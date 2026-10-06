/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/pipewire_pipeline.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <algorithm>
#include <array>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string_view>

static void runtimeReady(void *) {
  std::cerr << "product:runtime-ready\n" << std::flush;
}

static int runProduct(const char *preset, std::string_view scenario) {
  auto configuration = pipetune::OutputConfiguration{};
  for (auto index = 0U; index < 2; ++index) {
    const auto name = "pipetune_product_device_" + std::to_string(index);
    auto appended = pipetune::appendConfiguredOutput(configuration, {
        .id = name, .enabled = true,
        .device = {.identity = {.api = "node", .location = name, .port = name,
            .vendor = {}, .product = {}, .serial = {}}, .name = name, .profile = {},
            .channelPositions = {"FL", "FR"}}});
    if (!appended.error.empty()) return 1;
    configuration = std::move(appended.configuration);
  }
  configuration.mode = pipetune::OutputMode::multiple;
  if (scenario == "product-slots") {
    const auto &a = configuration.outputs[0].id;
    const auto &b = configuration.outputs[1].id;
    configuration.channels = {{b, 1, ""}, {b, 0, ""}, {}, {a, 1, ""}, {a, 0, ""}};
  }
  if (scenario == "product-sixteen") configuration.channels.resize(16);
  if (scenario == "product-disconnected") {
    configuration.outputs[0].device.identity.location += ".missing";
    configuration.outputs[1].enabled = false;
  }
  const auto width = pipetune::outputDspChannelCount(configuration);
  const auto build = pipetune::PipelineBuildOptions{.sampleRate = 48000, .maxChannels = width, .maxFrames = 8192};
  auto pipeline = std::unique_ptr<pipetune::DspPipeline>{};
  if (preset == nullptr) pipeline = std::move(pipetune::createBypassDspPipeline(build).pipeline);
  else {
    auto loaded = pipetune::loadDspPipeline(preset, build);
    if (loaded.pipeline == nullptr) { std::cerr << loaded.error << '\n'; return 1; }
    pipeline = std::move(loaded.pipeline);
  }
  const auto result = pipetune::runPipeWirePipeline(std::move(pipeline), {
      .filterName = "pipetune_product_input", .filterDescription = "PipeTune Multiple Outputs",
      .initialPresetPath = preset == nullptr ? "" : preset, .initialConfigurationError = {},
      .controlSocketPath = {}, .dspSampleRate = 48000,
      .ratePolicy = {.mode = pipetune::SampleRateMode::fixed, .fixedRate = 48000,
          .enforcement = pipetune::SampleRateEnforcement::suggest},
      .channelCount = width, .maxFrames = 8192, .ringCapacityFrames = 16384,
      .readyCallback = runtimeReady, .readyUserData = nullptr,
      .inputChannelCount = 2, .outputConfiguration = configuration},
      pipetune::PipeWireRunMode::untilInterrupted);
  if (!result.success) std::cerr << result.error << '\n';
  return result.success && result.processingErrors == 0 ? 0 : 1;
}

struct AudioTest;
struct TestOutput {
  AudioTest *test = nullptr;
  pw_stream *stream = nullptr;
  spa_hook listener = {};
  unsigned index = 0;
  bool ready = false;
  bool matched = false;
  std::uint64_t frames = 0;
};

struct AudioTest {
  pw_main_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  std::array<TestOutput, 3> outputs;
  pw_stream *source = nullptr;
  spa_hook sourceListener = {};
  bool matrix = false;
  bool slots = false;
  bool volume = false;
  bool disconnected = false;
  unsigned stage = 0;
  std::array<std::array<std::uint64_t, 2>, 4> stageFrames = {};
  pw_registry *registry = nullptr;
  spa_hook registryListener = {};
  pw_node *control = nullptr;
  bool announced = false;
  std::uint64_t produced = 0;
  std::string error;
};

static void fail(AudioTest &test, std::string error) {
  test.error = std::move(error);
  pw_main_loop_quit(test.loop);
}

static bool connectStereo(pw_stream *stream, pw_direction direction, pw_stream_flags flags) {
  auto format = spa_audio_info_raw{};
  format.format = SPA_AUDIO_FORMAT_F32P;
  format.rate = 48000;
  format.channels = 2;
  format.position[0] = SPA_AUDIO_CHANNEL_FL;
  format.position[1] = SPA_AUDIO_CHANNEL_FR;
  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto *parameter = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
  return pw_stream_connect(stream, direction, PW_ID_ANY, flags, &parameter, 1) >= 0;
}

static void outputState(void *data, pw_stream_state, pw_stream_state state, const char *error) {
  auto &output = *static_cast<TestOutput *>(data);
  if (state == PW_STREAM_STATE_ERROR) return fail(*output.test, error == nullptr ? "output failed" : error);
  if (state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING) output.ready = true;
  auto &test = *output.test;
  if (!test.announced && std::all_of(test.outputs.begin(), test.outputs.end(), [](const auto &item) { return item.ready; })) {
    test.announced = true;
    std::cerr << "product:devices-ready\n" << std::flush;
  }
}

static void setMasterVolume(AudioTest &test) {
  if (test.control == nullptr) return fail(test, "public input is unavailable for master control");
  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto gain = test.stage == 1 ? 0.5F : 1.0F;
  const auto volumes = std::array<float, 2>{gain, gain};
  const auto *parameter = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volume, SPA_POD_Float(1.0F), SPA_PROP_mute, SPA_POD_Bool(test.stage == 2),
      SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, volumes.size(), volumes.data())));
  if (pw_node_set_param(test.control, SPA_PARAM_Props, 0, parameter) < 0) fail(test, "cannot change master volume");
}

static void capture(void *data) {
  auto &output = *static_cast<TestOutput *>(data);
  auto &test = *output.test;
  auto *queued = pw_stream_dequeue_buffer(output.stream);
  if (queued == nullptr) return;
  auto valid = queued->buffer != nullptr && queued->buffer->n_datas == 2;
  auto matches = true;
  auto frames = std::uint32_t{0};
  for (auto channel = 0U; valid && channel < 2; ++channel) {
    const auto &plane = queued->buffer->datas[channel];
    valid = plane.chunk != nullptr && plane.data != nullptr && plane.chunk->offset <= plane.maxsize &&
        plane.chunk->size <= plane.maxsize - plane.chunk->offset && plane.chunk->stride == sizeof(float);
    if (!valid) break;
    frames = plane.chunk->size / sizeof(float);
    auto expected = output.index == 0 ? (channel == 0 ? 0.25F : 0.5F) :
        output.index == 1 && test.matrix ? (channel == 0 ? 0.25F : -0.5F) : 0.0F;
    if (test.slots && output.index < 2) expected = output.index == 0 ?
        (channel == 0 ? 0.25F : -0.5F) : (channel == 0 ? 0.5F : 0.25F);
    if (test.volume && output.index < 2) {
      expected += 0.125F;
      expected *= test.stage == 2 ? 0.0F : test.stage == 1 ? 0.5F : 1.0F;
    }
    for (auto frame = 0U; frame < frames; ++frame) {
      auto sample = 0.0F;
      std::memcpy(&sample, static_cast<const char *>(plane.data) + plane.chunk->offset + frame * sizeof(float), sizeof(float));
      if (test.disconnected && sample != 0) fail(test, "unresolved or disabled output received fallback audio");
      if (output.index == 2 && sample != 0) fail(test, "unselected device received audio");
      matches = matches && sample == expected;
    }
  }
  queued->size = frames;
  pw_stream_queue_buffer(output.stream, queued);
  if (!valid) return fail(test, "invalid stereo capture buffer");
  if (output.index == 2) return;
  // The product deliberately fades in after its initial bridge delay. Require
  // exact continuous PCM after the first complete block reaches unity gain.
  if (output.matched && frames != 0 && !matches) return fail(test, "steady product output differs on device " + std::to_string(output.index));
  if (matches && frames != 0) { output.matched = true; output.frames += frames; }
  if (test.outputs[0].frames >= 32768 && test.outputs[1].frames >= 32768) {
    test.stageFrames[test.stage] = {test.outputs[0].frames, test.outputs[1].frames};
    if (!test.volume || test.stage == 3) pw_main_loop_quit(test.loop);
    else {
      ++test.stage;
      for (auto &item : test.outputs) { item.frames = 0; item.matched = false; }
      setMasterVolume(test);
    }
  }
}

static void produce(void *data) {
  auto &test = *static_cast<AudioTest *>(data);
  auto *queued = pw_stream_dequeue_buffer(test.source);
  if (queued == nullptr) return;
  if (queued->buffer == nullptr || queued->buffer->n_datas != 2) return fail(test, "invalid source buffer");
  auto frames = static_cast<std::uint32_t>(queued->requested == 0 ? 256 : queued->requested);
  for (auto channel = 0U; channel < 2; ++channel) {
    const auto &plane = queued->buffer->datas[channel];
    if (plane.data == nullptr || plane.chunk == nullptr) return fail(test, "unmapped source plane");
    frames = std::min(frames, static_cast<std::uint32_t>(plane.maxsize / sizeof(float)));
  }
  for (auto channel = 0U; channel < 2; ++channel) {
    auto &plane = queued->buffer->datas[channel];
    std::fill_n(static_cast<float *>(plane.data), frames, channel == 0 ? 0.25F : 0.5F);
    plane.chunk->offset = 0;
    plane.chunk->size = frames * sizeof(float);
    plane.chunk->stride = sizeof(float);
  }
  queued->size = frames;
  pw_stream_queue_buffer(test.source, queued);
  test.produced += frames;
  if (test.disconnected && test.produced >= 65536) pw_main_loop_quit(test.loop);
}

static void startSource(void *data, int) {
  auto &test = *static_cast<AudioTest *>(data);
  if (test.source != nullptr) return fail(test, "source started twice");
  test.source = pw_stream_new(test.core, "Product test signal", pw_properties_new(
      PW_KEY_NODE_NAME, "pipetune_product_signal", PW_KEY_TARGET_OBJECT, "pipetune_product_input",
      PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
      "node.dont-fallback", "true", "node.dont-move", "true", "stream.dont-remix", "true", nullptr));
  static const auto events = [] { auto value = pw_stream_events{}; value.version = PW_VERSION_STREAM_EVENTS; value.process = produce; return value; }();
  if (test.source == nullptr) return fail(test, "cannot create source");
  pw_stream_add_listener(test.source, &test.sourceListener, &events, &test);
  if (!connectStereo(test.source, PW_DIRECTION_OUTPUT,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT))) fail(test, "cannot connect source");
}

static int runAudio(std::string_view scenario) {
  pw_init(nullptr, nullptr);
  auto test = AudioTest{};
  test.matrix = scenario != "product-bypass";
  test.slots = scenario == "product-slots";
  test.volume = scenario == "product-volume";
  test.disconnected = scenario == "product-disconnected";
  test.loop = pw_main_loop_new(nullptr);
  if (test.loop == nullptr) return 1;
  // Block and register the control signal before PipeWire creates data threads.
  auto *signal = pw_loop_add_signal(pw_main_loop_get_loop(test.loop), SIGUSR1, startSource, &test);
  if (signal == nullptr) return 1;
  test.context = pw_context_new(pw_main_loop_get_loop(test.loop), nullptr, 0);
  if (test.context == nullptr) return 1;
  test.core = pw_context_connect(test.context, nullptr, 0);
  if (test.core == nullptr) return 1;
  test.registry = pw_core_get_registry(test.core, PW_VERSION_REGISTRY, 0);
  static const auto registryEvents = [] {
    auto value = pw_registry_events{};
    value.version = PW_VERSION_REGISTRY_EVENTS;
    value.global = [](void *data, std::uint32_t id, std::uint32_t, const char *type, std::uint32_t version, const spa_dict *props) {
      auto &test = *static_cast<AudioTest *>(data);
      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == nullptr || test.control != nullptr) return;
      const auto *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (name != nullptr && std::string_view(name) == "pipetune_product_input")
        test.control = static_cast<pw_node *>(pw_registry_bind(test.registry, id, type, std::min<std::uint32_t>(version, PW_VERSION_NODE), 0));
    };
    return value;
  }();
  if (test.registry == nullptr) return 1;
  pw_registry_add_listener(test.registry, &test.registryListener, &registryEvents, &test);
  static const auto events = [] {
    auto value = pw_stream_events{}; value.version = PW_VERSION_STREAM_EVENTS;
    value.state_changed = outputState; value.process = capture; return value;
  }();
  for (auto index = 0U; index < test.outputs.size(); ++index) {
    auto &output = test.outputs[index]; output.test = &test; output.index = index;
    const auto name = "pipetune_product_device_" + std::to_string(index);
    output.stream = pw_stream_new(test.core, name.c_str(), pw_properties_new(
        PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, name.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
        PW_KEY_NODE_GROUP, "pipewire.dummy", PW_KEY_NODE_VIRTUAL, "false",
        PW_KEY_PRIORITY_SESSION, index == 2 ? "2000" : "1000",
        "node.want-driver", "true", "node.pause-on-idle", "false",
        "audio.channels", "2", "audio.position", "[ FL FR ]", nullptr));
    if (output.stream == nullptr) return 1;
    pw_stream_add_listener(output.stream, &output.listener, &events, &output);
    if (!connectStereo(output.stream, PW_DIRECTION_INPUT, PW_STREAM_FLAG_MAP_BUFFERS)) return 1;
  }
  auto *timer = pw_loop_add_timer(pw_main_loop_get_loop(test.loop), [](void *data, std::uint64_t) {
    fail(*static_cast<AudioTest *>(data), "product PCM test timed out");
  }, &test);
  auto deadline = timespec{.tv_sec = 15, .tv_nsec = 0}; auto interval = timespec{};
  if (timer == nullptr || signal == nullptr ||
      pw_loop_update_timer(pw_main_loop_get_loop(test.loop), timer, &deadline, &interval, false) < 0) return 1;
  pw_main_loop_run(test.loop);
  if (test.source != nullptr) pw_stream_destroy(test.source);
  for (auto &output : test.outputs) pw_stream_destroy(output.stream);
  if (test.control != nullptr) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(test.control));
  spa_hook_remove(&test.registryListener);
  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(test.registry));
  pw_loop_destroy_source(pw_main_loop_get_loop(test.loop), timer);
  pw_loop_destroy_source(pw_main_loop_get_loop(test.loop), signal);
  pw_core_disconnect(test.core); pw_context_destroy(test.context); pw_main_loop_destroy(test.loop); pw_deinit();
  if (!test.error.empty()) std::cerr << test.error << '\n';
  std::cout << "{\"receivedFrames\":[" << test.outputs[0].frames << ',' << test.outputs[1].frames
            << "],\"producedFrames\":" << test.produced << ",\"stages\":[";
  for (auto stage = 0U; stage <= test.stage; ++stage) {
    if (stage != 0) std::cout << ',';
    std::cout << '[' << test.stageFrames[stage][0] << ',' << test.stageFrames[stage][1] << ']';
  }
  std::cout << "]}\n";
  return test.error.empty() ? 0 : 1;
}

int main(int argc, char **argv) {
  if (argc < 3) return 2;
  if (std::string_view(argv[1]) == "audio") return runAudio(argv[2]);
  if (std::string_view(argv[1]) == "runtime" && argc == 4)
    return runProduct(std::string_view(argv[2]) == "bypass" ? nullptr : argv[2], argv[3]);
  return 2;
}
