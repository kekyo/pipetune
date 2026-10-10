/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/pipewire_pipeline.h"
#include "pipetune/control_socket.h"
#include "pipewire_graph_clock.h"
#include "pipewire_output_paths.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/latency-utils.h>
#include <spa/param/props.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <string_view>

static bool rejectRestoredWidth = false;
static bool stallWideOutput = false;
static bool sawWideOutput = false;
static unsigned outputFailures = 0;
struct DeferredOutputFailure {
  pw_loop *loop;
  spa_source *event;
  pw_stream *stream;
};
struct UnconfiguredOutput {
  pw_stream *stream;
  spa_hook listener = {};
  bool registered = false;
  bool negotiated = false;
};

// Only this test executable wraps the public API. Fail one real stream
// connection after a wide layout has played, then report a negotiation error.
// Restoring the old width and the final retry use the real API.
// A separate scenario leaves one exported stream unconfigured until timeout.
extern "C" int __real_pw_stream_connect(pw_stream *, pw_direction, std::uint32_t,
    pw_stream_flags, const spa_pod **, std::uint32_t);
extern "C" int __wrap_pw_stream_connect(pw_stream *stream, pw_direction direction,
    std::uint32_t target, pw_stream_flags flags, const spa_pod **parameters, std::uint32_t count) {
  if (stallWideOutput && direction == PW_DIRECTION_OUTPUT && count != 0) {
    auto format = spa_audio_info_raw{};
    if (spa_format_audio_raw_parse(parameters[0], &format) >= 0 && format.channels == 16) {
      stallWideOutput = false;
      // Export a real stream outside the session manager's audio classes.
      // Its format remains unconfigured until the product's deadline expires.
      const auto item = spa_dict_item{PW_KEY_MEDIA_CLASS, "PipeTune/Test"};
      const auto properties = SPA_DICT_INIT(&item, 1);
      const auto updated = pw_stream_update_properties(stream, &properties);
      if (updated < 0) return updated;
      auto observation = std::make_unique<UnconfiguredOutput>(UnconfiguredOutput{.stream = stream});
      static const auto events = [] {
        auto value = pw_stream_events{};
        value.version = PW_VERSION_STREAM_EVENTS;
        value.state_changed = [](void *data, pw_stream_state, pw_stream_state state, const char *) {
          auto &observation = *static_cast<UnconfiguredOutput *>(data);
          if (state == PW_STREAM_STATE_PAUSED && pw_stream_get_node_id(observation.stream) != PW_ID_ANY)
            observation.registered = true;
        };
        value.param_changed = [](void *data, std::uint32_t id, const spa_pod *parameter) {
          if (id == SPA_PARAM_Format && parameter != nullptr)
            static_cast<UnconfiguredOutput *>(data)->negotiated = true;
        };
        value.destroy = [](void *data) {
          auto observation = std::unique_ptr<UnconfiguredOutput>(static_cast<UnconfiguredOutput *>(data));
          spa_hook_remove(&observation->listener);
          if (observation->registered && !observation->negotiated)
            std::cerr << "product:unconfigured-output-retired\n" << std::flush;
        };
        return value;
      }();
      pw_stream_add_listener(stream, &observation->listener, &events, observation.get());
      static_cast<void>(observation.release());
      const auto result = __real_pw_stream_connect(stream, direction, target, flags, parameters, count);
      if (result >= 0) std::cerr << "product:unconfigured-output-stream\n" << std::flush;
      return result;
    }
  }
  if (rejectRestoredWidth && direction == PW_DIRECTION_OUTPUT && count != 0) {
    auto format = spa_audio_info_raw{};
    if (spa_format_audio_raw_parse(parameters[0], &format) >= 0) {
      if (format.channels == 16) sawWideOutput = true;
      else if (sawWideOutput && format.channels == 4) {
        if (++outputFailures == 1) {
          std::cerr << "product:injected-output-failure\n" << std::flush;
          return -EIO;
        }
        rejectRestoredWidth = false;
        const auto result = __real_pw_stream_connect(stream, direction, target, flags, parameters, count);
        if (result < 0) return result;
        auto *loop = pw_context_get_main_loop(pw_core_get_context(pw_stream_get_core(stream)));
        auto failure = std::make_unique<DeferredOutputFailure>(DeferredOutputFailure{loop, nullptr, stream});
        failure->event = pw_loop_add_event(loop, [](void *data, std::uint64_t) {
          auto failure = std::unique_ptr<DeferredOutputFailure>(static_cast<DeferredOutputFailure *>(data));
          std::cerr << "product:injected-output-negotiation-error\n" << std::flush;
          pw_stream_set_error(failure->stream, -EIO, "injected output negotiation error");
          pw_loop_destroy_source(failure->loop, failure->event);
        }, failure.get());
        if (failure->event == nullptr) return -ENOMEM;
        pw_loop_signal_event(loop, failure->event);
        static_cast<void>(failure.release());
        return result;
      }
    }
  }
  return __real_pw_stream_connect(stream, direction, target, flags, parameters, count);
}

static void runtimeReady(void *) {
  std::cerr << "product:runtime-ready\n" << std::flush;
}

static int runProduct(const char *preset, std::string_view scenario) {
  rejectRestoredWidth = scenario == "product-output-change";
  stallWideOutput = scenario == "product-output-timeout";
  const auto *socket = std::getenv("PIPETUNE_PRODUCT_SOCKET");
  const auto control = scenario == "product-controls" || scenario.starts_with("product-output-");
  if (control && socket == nullptr) return 2;
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
  if (scenario == "product-output-status-single") configuration.mode = pipetune::OutputMode::single;
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
      .controlSocketPath = control ? socket : "", .dspSampleRate = 48000,
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
  std::uint64_t zeroFrames = 0;
  std::uint64_t longestSilence = 0;
  std::int64_t previousTag = -1;
  std::int64_t blockTime = 0;
  std::uint32_t blockFrames = 0;
};

struct AudioTest {
  pw_main_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  std::array<TestOutput, 3> outputs;
  pw_stream *source = nullptr;
  std::string inputName = "pipetune_product_input";
  spa_hook sourceListener = {};
  bool matrix = false;
  bool slots = false;
  bool volume = false;
  bool daemonVolume = false;
  bool restoredVolume = false;
  float savedMasterGain = 0.5F;
  bool disconnected = false;
  bool reconnect = false;
  bool profile = false;
  bool restart = false;
  bool restartMuted = false;
  bool restartResumed = false;
  bool latency = false;
  bool latencyUnits = false;
  bool latencyReconnect = false;
  bool intentionalDelay = false;
  bool controls = false;
  bool outputChange = false;
  bool outputTimeout = false;
  bool recovery = false;
  bool graphClock = false;
  bool clockUnavailable = false;
  bool clockReconnect = false;
  bool clockSnapshot = false;
  std::array<std::uint64_t, 2> clockGenerations = {};
  unsigned initialGraphRate = 48000;
  unsigned graphQuantum = 256;
  std::vector<pipetune::PipeWireNodeClock> clocks;
  std::vector<pipetune::PipeWireOutputPath> paths;
  std::array<unsigned, 9> observedGraphRates = {};
  std::array<std::uint64_t, 9> observedGraphQuanta = {};
  std::uint64_t singleModeFrames = 0;
  bool inventory = false;
  bool awaitingControl = false;
  bool latencySettled = false;
  std::uint32_t declaredLatency = 63;
  std::array<std::int64_t, 3> compensation = {};
  spa_source *graphEvent = nullptr;
  std::uint32_t publicInputId = PW_ID_ANY;
  unsigned stage = 0;
  std::array<std::array<std::uint64_t, 2>, 12> stageFrames = {};
  std::array<std::array<std::uint64_t, 2>, 12> recoverySilence = {};
  std::array<double, 12> recoveryMilliseconds = {};
  std::chrono::steady_clock::time_point recoveryStarted = std::chrono::steady_clock::now();
  pw_registry *registry = nullptr;
  spa_hook registryListener = {};
  pw_node *control = nullptr;
  spa_hook controlListener = {};
  float visibleGain = -1.0F;
  bool visibleMute = false;
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

static void changeOutputGraph(void *data, std::uint64_t);

// Exactly representable channel offsets and frame tags permit PCM delay
// measurements without relying on callback arrival order or startup silence.
static constexpr auto tagPeriod = std::int64_t{524287};
static constexpr auto channelOffset = std::int64_t{524288};
static constexpr auto sampleScale = 268435456.0F;

static void captureLatency(TestOutput &output, pw_buffer &buffer) {
  auto &test = *output.test;
  auto frames = std::uint32_t{UINT32_MAX};
  if (buffer.buffer == nullptr || buffer.buffer->n_datas != 2)
    return fail(test, "invalid latency capture buffer");
  for (const auto &plane : std::span(buffer.buffer->datas, 2)) {
    if (plane.data == nullptr || plane.chunk == nullptr || plane.chunk->offset > plane.maxsize ||
        plane.chunk->size > plane.maxsize - plane.chunk->offset || plane.chunk->stride != sizeof(float))
      return fail(test, "invalid latency capture plane");
    frames = std::min(frames, static_cast<std::uint32_t>(plane.chunk->size / sizeof(float)));
  }
  auto validBlock = frames != 0;
  for (auto frame = 0U; frame < frames; ++frame) {
    auto tags = std::array<std::int64_t, 2>{};
    auto validFrame = true;
    for (auto channel = 0U; channel < 2; ++channel) {
      const auto &plane = buffer.buffer->datas[channel];
      auto sample = 0.0F;
      std::memcpy(&sample, static_cast<const char *>(plane.data) + plane.chunk->offset + frame * sizeof(float), sizeof(float));
      if (output.index == 2) {
        if (sample != 0.0F) return fail(test, "unselected device received latency signal");
        continue;
      }
      if (output.index == 1 && channel == 1) sample = -sample;
      const auto scaled = sample * sampleScale;
      if (!std::isfinite(scaled) || scaled < (channel + 1) * channelOffset ||
          scaled >= (channel + 1) * channelOffset + tagPeriod || std::floor(scaled) != scaled) {
        validFrame = false;
      } else tags[channel] = static_cast<std::int64_t>(scaled) - (channel + 1) * channelOffset;
    }
    if (output.index == 2) continue;
    validFrame = validFrame && tags[0] == tags[1];
    const auto consecutive = output.previousTag < 0 || tags[0] == (output.previousTag + 1) % tagPeriod;
    if (test.latencySettled && (!validFrame || !consecutive)) {
      if (!test.latencyReconnect || test.stage == 0)
        return fail(test, "steady latency PCM lost channel identity or frame order");
      // Rebuilding the distribution intentionally fades and discards queued
      // audio. Start the consecutive measurement window after that boundary.
      test.latencySettled = false;
      for (auto &item : test.outputs) item.frames = 0;
    }
    validBlock = validBlock && validFrame && consecutive;
    output.previousTag = validFrame ? tags[0] : -1;
  }
  buffer.size = frames;
  if (output.index == 2) return;
  output.matched = validBlock;
  output.blockTime = static_cast<std::int64_t>(buffer.time);
  output.blockFrames = frames;
  if (validBlock && test.latencySettled) output.frames += frames;
  const auto &a = test.outputs[0];
  const auto &b = test.outputs[1];
  const auto single = test.latencyReconnect && test.stage == 1;
  if (!a.matched || (!single && !b.matched) || a.blockTime == 0 || (!single && b.blockTime == 0)) return;
  const auto elapsedFrames = std::llround(static_cast<double>(a.blockTime - b.blockTime) * 48000 / 1000000000);
  const auto relative = elapsedFrames + a.blockFrames - b.blockFrames + b.previousTag - a.previousTag +
      (test.intentionalDelay ? 48 : 0);
  const auto compensation = single ? -1 : (relative % tagPeriod + tagPeriod) % tagPeriod;
  const auto aligned = single || (compensation <= test.declaredLatency && test.declaredLatency - compensation <= 1);
  if (!test.latencySettled) {
    if (aligned) {
      test.latencySettled = true;
      for (auto &item : test.outputs) item.frames = 0;
    }
    return;
  }
  if (!aligned) {
    if (!test.latencyReconnect || test.stage == 0)
      return fail(test, "steady product delay compensation differs from the device latency");
    test.latencySettled = false;
    for (auto &item : test.outputs) item.frames = 0;
    return;
  }
  if (a.frames < 32768 || (!single && b.frames < 32768)) return;
  auto latencies = std::array<double, 2>{};
  for (auto index = 0U; index < 2; ++index) {
    const auto id = "pipetune_product_device_" + std::to_string(index);
    const auto path = std::find_if(test.paths.begin(), test.paths.end(),
        [&](const auto &item) { return item.outputId == id; });
    if (single && index == 1) {
      if (path != test.paths.end()) return;
    } else {
      if (path == test.paths.end() || path->activity != pipetune::OutputPathActivity::active ||
          path->targetNodeId != pw_stream_get_node_id(test.outputs[index].stream) ||
          path->targetSerial == 0 || path->streamSerial == 0 || !path->reportedLatencyNanoseconds) return;
      latencies[index] = *path->reportedLatencyNanoseconds;
    }
  }
  if (!single && std::abs(latencies[1] - latencies[0] - test.declaredLatency * 1000000000.0 / 48000) > 1.0) return;
  test.stageFrames[test.stage] = {a.frames, b.frames};
  test.compensation[test.stage] = compensation;
  if (test.stage == 2) return static_cast<void>(pw_main_loop_quit(test.loop));
  ++test.stage;
  test.declaredLatency = test.latencyReconnect ? (test.stage == 1 ? 0 : 63) : test.stage == 1 ? 127 : 31;
  test.latencySettled = false;
  for (auto &item : test.outputs) { item.frames = 0; item.matched = false; item.previousTag = -1; }
  pw_loop_signal_event(pw_main_loop_get_loop(test.loop), test.graphEvent);
}

static void setMasterVolume(AudioTest &test) {
  if (test.control == nullptr) return fail(test, "public input is unavailable for master control");
  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  if (test.restoredVolume) {
    // Unmute without sending a gain: the recovered value must remain in use.
    const auto *parameter = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_POD_Bool(false)));
    if (pw_node_set_param(test.control, SPA_PARAM_Props, 0, parameter) < 0) fail(test, "cannot unmute restored master volume");
    return;
  }
  const auto gain = test.daemonVolume ? test.savedMasterGain : test.reconnect || test.stage == 1 ? 0.5F : 1.0F;
  const auto volumes = std::array<float, 2>{gain, gain};
  const auto *parameter = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volume, SPA_POD_Float(1.0F), SPA_PROP_mute, SPA_POD_Bool(test.restartMuted ? test.stage < 3 : !test.reconnect && test.stage == 2),
      SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, volumes.size(), volumes.data())));
  if (pw_node_set_param(test.control, SPA_PARAM_Props, 0, parameter) < 0) fail(test, "cannot change master volume");
}

static void capture(void *data) {
  auto &output = *static_cast<TestOutput *>(data);
  auto &test = *output.test;
  auto *queued = pw_stream_dequeue_buffer(output.stream);
  if (queued == nullptr) return;
  if (test.latency) {
    captureLatency(output, *queued);
    pw_stream_queue_buffer(output.stream, queued);
    return;
  }
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
    if ((test.volume || test.reconnect) && output.index < 2) {
      expected += 0.125F;
      expected *= test.reconnect ? (test.stage == 0 ? 1.0F : 0.5F) :
          test.stage == 2 ? 0.0F : test.daemonVolume && test.stage > 0 ? test.savedMasterGain : test.stage == 1 ? 0.5F : 1.0F;
    }
    if (test.restartMuted && (test.stage == 1 || test.stage == 2)) expected = 0.0F;
    if (test.profile && test.stage == 3 && output.index == 0) expected = 0.0F;
    if (test.controls && output.index < 2) {
      if (test.stage == 3 && output.index == 1) expected = 0.0F;
      if (test.stage >= 4) expected += test.stage == 4 ? 0.125F : 0.25F;
      if (test.stage == 6) expected = 0.0F;
    }
    if (test.outputChange) {
      if (test.stage >= 1 && test.stage <= 5 && output.index < 2)
        expected = channel == 0 ? 0.25F : output.index == 0 ? -0.5F : 0.5F;
      if (test.stage >= 3 && test.stage <= 5 && output.index == 0) expected = 0.0F;
      if (test.stage == 7) expected = output.index == 2 ? (channel == 0 ? 0.25F : 0.5F) : 0.0F;
    }
    if (test.outputTimeout && test.stage == 2 && output.index == 1) expected = 0.0F;
    if (test.recovery && test.stage == 7 && output.index == 0) expected = 0.0F;
    if (test.recovery && (test.stage == 9 || test.stage == 10))
      expected = output.index == 2 ? (channel == 0 ? 0.25F : 0.5F) : 0.0F;
    for (auto frame = 0U; frame < frames; ++frame) {
      auto sample = 0.0F;
      std::memcpy(&sample, static_cast<const char *>(plane.data) + plane.chunk->offset + frame * sizeof(float), sizeof(float));
      if (test.recovery && channel == 0 && !test.awaitingControl) {
        output.zeroFrames = sample == 0.0F ? output.zeroFrames + 1 : 0;
        output.longestSilence = std::max(output.longestSilence, output.zeroFrames);
      }
      if (test.restoredVolume && test.stage == 2 && sample != 0.0F)
        fail(test, "muted daemon restart emitted audio before restoring the controls");
      if (test.profile && test.stage == 3 && output.index == 0 && sample != 0)
        fail(test, "profile mismatch received audio");
      if (test.disconnected && sample != 0) fail(test, "unresolved or disabled output received fallback audio");
      // A Single-to-Multiple change may drain already queued Single audio.
      // The next verified steady window ends that transition allowance.
      if (output.index == 2 && sample != 0 &&
          !(test.recovery && (test.stage == 9 || test.stage == 10)) &&
          !(test.recovery && test.stage == 11 && !test.awaitingControl) &&
          !(test.outputChange && (test.stage >= 7 || test.awaitingControl))) fail(test, "unselected device received audio");
      if (test.recovery && test.stage == 7 && output.index == 0 && sample != 0)
        fail(test, "mismatched recovery profile received audio");
      matches = matches && (test.controls && expected != 0.0F ? std::abs(sample - expected) < 0.000001F : sample == expected);
    }
  }
  queued->size = frames;
  pw_stream_queue_buffer(output.stream, queued);
  if (!valid) return fail(test, "invalid stereo capture buffer");
  if (test.recovery) {
    if (test.awaitingControl) return;
    const auto required = test.stage == 2 ? 0U :
        test.stage == 1 || test.stage == 3 || test.stage == 7 ? 2U :
        test.stage == 9 || test.stage == 10 ? 4U : 3U;
    if ((required & (1U << output.index)) == 0) return;
    output.frames = matches ? output.frames + frames : 0;
    for (auto index = 0U; index < test.outputs.size(); ++index)
      if ((required & (1U << index)) != 0 && test.outputs[index].frames < 32768) return;
    test.stageFrames[test.stage] = {test.outputs[0].frames, test.outputs[1].frames};
    test.recoverySilence[test.stage] = {test.outputs[0].longestSilence, test.outputs[1].longestSilence};
    test.recoveryMilliseconds[test.stage] = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - test.recoveryStarted).count();
    if (required == 4) test.singleModeFrames = test.outputs[2].frames;
    test.awaitingControl = true;
    std::cerr << "product:recovery-stage-" << test.stage << '\n' << std::flush;
    return;
  }
  if (output.index == 2 && !(test.outputChange && test.stage >= 7)) return;
  if (test.awaitingControl) return;
  if (test.outputChange && test.stage == 7) {
    if (output.index == 2) {
      test.singleModeFrames = matches ? test.singleModeFrames + frames : 0;
      if (test.singleModeFrames >= 32768) {
        test.awaitingControl = true;
        std::cerr << "product:controls-stage-7\n" << std::flush;
      }
    } else if (!matches) return fail(test, "single mode played on a non-default output");
    return;
  }
  if (test.restart && test.stage == 2 && !test.restartResumed) return;
  if (test.reconnect && !test.restart && (test.stage == 2 || (test.profile && test.stage == 3)) && output.index == 0) return;
  // Graph replacement permits a brief fade or silence. Require a fresh,
  // consecutive window of exact PCM after each device change rather than
  // counting samples from the old graph toward the recovered state.
  if (((test.reconnect && test.stage >= 2) || test.controls || test.outputChange || test.outputTimeout) && !matches) {
    output.matched = false;
    output.frames = 0;
    return;
  }
  // The product deliberately fades in after its initial bridge delay. Require
  // exact continuous PCM after the first complete block reaches unity gain.
  if (output.matched && frames != 0 && !matches) return fail(test, "steady product output differs on device " + std::to_string(output.index));
  if (matches && frames != 0) { output.matched = true; output.frames += frames; }
  if ((test.outputs[0].frames >= 32768 || (test.reconnect && !test.restart && (test.stage == 2 || (test.profile && test.stage == 3)))) && test.outputs[1].frames >= 32768 &&
      (!test.outputChange || test.stage != 8 || test.outputs[2].frames >= 32768)) {
    if (test.graphClock || test.clockUnavailable || test.clockReconnect) {
      if (!test.clockSnapshot) return;
      if (test.clockUnavailable && !test.clocks.empty()) return fail(test, "unavailable Profiler reported a graph clock");
      if (std::any_of(test.clocks.begin(), test.clocks.end(), [](const auto &clock) {
          return clock.nodeName == "pipetune_product_device_2";
        })) return fail(test, "idle unselected output retained a graph clock");
      for (auto index = 0U; index < 2; ++index) {
        const auto id = "pipetune_product_device_" + std::to_string(index);
        const auto path = std::find_if(test.paths.begin(), test.paths.end(),
            [&](const auto &item) { return item.outputId == id; });
        if (test.clockReconnect && test.stage == 2 && index == 0) {
          if (path != test.paths.end()) return;
        } else {
          if (path == test.paths.end() || path->activity != pipetune::OutputPathActivity::active ||
              path->targetNodeId != pw_stream_get_node_id(test.outputs[index].stream) ||
              path->targetSerial == 0 || path->streamSerial == 0) return;
          if (test.clockUnavailable) {
            if (path->reportedLatencyNanoseconds) return fail(test, "missing graph clock must leave latency unknown");
          } else if (!path->reportedLatencyNanoseconds) return;
        }
      }
    }
    if (test.clockReconnect) {
      const auto name = std::string_view("output.pipetune_product_input.distribution_pipetune_product_device_0");
      const auto clock = std::find_if(test.clocks.begin(), test.clocks.end(),
          [&](const auto &item) { return item.nodeName == name; });
      if (test.stage == 2) {
        if (clock != test.clocks.end()) return;
      } else {
        if (clock == test.clocks.end() || clock->nodeSerial == 0 || clock->rateNumerator != 1 ||
            clock->rateDenominator != 48000 || clock->quantum != 256) return;
        if (test.stage == 0) test.clockGenerations[0] = clock->nodeSerial;
        if (test.stage == 3) {
          if (clock->nodeSerial == test.clockGenerations[0]) return fail(test, "reconnected output retained its old graph clock generation");
          test.clockGenerations[1] = clock->nodeSerial;
        }
      }
    }
    if (test.clockUnavailable) {
      test.stageFrames[0] = {test.outputs[0].frames, test.outputs[1].frames};
      test.awaitingControl = true;
      std::cerr << "product:controls-stage-0\n" << std::flush;
      return;
    }
    if (test.daemonVolume) {
      if (test.produced < 32768) return;
      if (test.visibleGain != (test.stage == 0 ? 1.0F : test.savedMasterGain) || test.visibleMute != (test.stage == 2))
        return fail(test, "visible master controls do not match the daemon restart PCM stage");
      if (!test.restoredVolume && test.stage == 2) {
        test.stageFrames[test.stage] = {test.outputs[0].frames, test.outputs[1].frames};
        pw_main_loop_quit(test.loop);
        return;
      }
    }
    if (test.controls || test.outputChange || test.outputTimeout) {
      if (test.graphClock) {
        const auto expectedRate = test.stage == 0 ? test.initialGraphRate : test.stage < 8 ? 96000U : 48000U;
        // PipeWire scales the configured cycle size when the graph rate changes.
        const auto expectedQuantum = test.stage > 0 && test.stage < 8 ? test.graphQuantum * 2 : test.graphQuantum;
        auto latencies = std::array<double, 2>{};
        for (auto index = 0U; index < 2; ++index) {
          const auto name = "output.pipetune_product_input.distribution_pipetune_product_device_" + std::to_string(index);
          const auto clock = std::find_if(test.clocks.begin(), test.clocks.end(),
              [&](const auto &clock) { return clock.nodeName == name; });
          if (clock == test.clocks.end() || clock->nodeSerial == 0 || clock->rateNumerator != 1 ||
              clock->rateDenominator != expectedRate || clock->quantum != expectedQuantum) return;
          test.observedGraphRates[test.stage] = clock->rateDenominator;
          test.observedGraphQuanta[test.stage] = clock->quantum;
          const auto outputId = "pipetune_product_device_" + std::to_string(index);
          const auto path = std::find_if(test.paths.begin(), test.paths.end(),
              [&](const auto &item) { return item.outputId == outputId; });
          latencies[index] = *path->reportedLatencyNanoseconds;
        }
        const auto expectedDelay = (0.5 * expectedQuantum + 64) * 1000000000.0 / expectedRate + 1000000;
        if (std::abs(latencies[1] - latencies[0] - expectedDelay) > 1.0) return;
      }
      test.stageFrames[test.stage] = {test.outputs[0].frames, test.outputs[1].frames};
      if (!test.graphClock && test.stage == (test.outputTimeout ? 2U : 8U)) pw_main_loop_quit(test.loop);
      else {
        test.awaitingControl = true;
        std::cerr << "product:controls-stage-" << test.stage << '\n' << std::flush;
      }
      return;
    }
    if (test.restart && test.stage == 2 &&
        (test.visibleGain != 0.5F || test.visibleMute != test.restartMuted))
      return fail(test, "master controls were not restored after the policy restart");
    test.stageFrames[test.stage] = {test.outputs[0].frames, test.outputs[1].frames};
    if ((!test.volume && !test.reconnect) || test.stage == (test.profile ? 4U : test.restart && !test.restartMuted ? 2U : 3U)) pw_main_loop_quit(test.loop);
    else {
      ++test.stage;
      for (auto &item : test.outputs) { item.frames = 0; item.matched = false; }
      if (test.restart && test.stage == 2)
        std::cerr << "product:restart-ready\n" << std::flush;
      else if (test.reconnect && !test.restart && test.stage >= 2)
        pw_loop_signal_event(pw_main_loop_get_loop(test.loop), test.graphEvent);
      else setMasterVolume(test);
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
    if (test.latency) {
      for (auto frame = 0U; frame < frames; ++frame)
        static_cast<float *>(plane.data)[frame] = static_cast<float>((channel + 1) * channelOffset +
            (test.produced + frame) % tagPeriod) / sampleScale;
    } else std::fill_n(static_cast<float *>(plane.data), frames,
        test.controls && test.stage == 6 ? 0.0F : channel == 0 ? 0.25F : 0.5F);
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
      PW_KEY_NODE_NAME, "pipetune_product_signal", PW_KEY_TARGET_OBJECT, test.inputName.c_str(),
      PW_KEY_MEDIA_CATEGORY, "Playback", PW_KEY_MEDIA_ROLE, "Music",
      // This fixture pins its target and waits across intentional rate
      // rebuilds. Without linger, WirePlumber 0.5 destroys a pinned stream
      // when its target temporarily disappears and fallback is forbidden.
      "node.dont-fallback", "true", "node.dont-move", "true", "node.linger", "true", nullptr));
  static const auto events = [] { auto value = pw_stream_events{}; value.version = PW_VERSION_STREAM_EVENTS; value.process = produce; return value; }();
  if (test.source == nullptr) return fail(test, "cannot create source");
  pw_stream_add_listener(test.source, &test.sourceListener, &events, &test);
  if (!connectStereo(test.source, PW_DIRECTION_OUTPUT,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_AUTOCONNECT))) fail(test, "cannot connect source");
}

static void outputParameterChanged(void *data, std::uint32_t id, const spa_pod *parameter) {
  auto &output = *static_cast<TestOutput *>(data);
  if ((!output.test->latency && !output.test->graphClock) || (id != SPA_PARAM_Format && id != SPA_PARAM_Latency)) return;
  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  auto downstream = spa_latency_info{};
  downstream.direction = SPA_DIRECTION_INPUT;
  downstream.min_rate = downstream.max_rate = output.index == 1 ? output.test->declaredLatency : 0;
  if (output.test->latencyUnits && output.index == 1) {
    // At 48 kHz / 256 frames the midpoints contribute eight frames from
    // quantum, declaredLatency - 16 frames from rate, and eight from ns.
    downstream.min_quantum = 0;
    downstream.max_quantum = 0.0625F;
    downstream.min_rate = output.test->declaredLatency - 24;
    downstream.max_rate = output.test->declaredLatency - 8;
    downstream.min_ns = 0;
    // Round up: 1.0.5 truncates ns to frames before combine's second rounding.
    downstream.max_ns = 333334;
  }
  if (output.test->graphClock && output.index == 1) {
    downstream.min_quantum = downstream.max_quantum = 0.5F;
    downstream.min_rate = downstream.max_rate = 64;
    downstream.min_ns = downstream.max_ns = 1000000;
  }
  auto upstream = spa_latency_info{};
  upstream.direction = SPA_DIRECTION_OUTPUT;
  auto parameters = std::array<const spa_pod *, 2>{spa_latency_build(&builder, SPA_PARAM_Latency, &downstream), nullptr};
  if (id == SPA_PARAM_Latency && parameter != nullptr && spa_latency_parse(parameter, &upstream) >= 0 &&
      upstream.direction == SPA_DIRECTION_OUTPUT) parameters[1] = parameter;
  else {
    upstream = {};
    upstream.direction = SPA_DIRECTION_OUTPUT;
    parameters[1] = spa_latency_build(&builder, SPA_PARAM_Latency, &upstream);
  }
  if (pw_stream_update_params(output.stream, parameters.data(), parameters.size()) < 0)
    fail(*output.test, "cannot publish device latency");
}

static bool createTestOutput(TestOutput &output) {
  static const auto events = [] {
    auto value = pw_stream_events{}; value.version = PW_VERSION_STREAM_EVENTS;
    value.state_changed = outputState; value.process = capture;
    value.param_changed = outputParameterChanged; return value;
  }();
  const auto name = "pipetune_product_device_" + std::to_string(output.index);
  output.stream = pw_stream_new(output.test->core, name.c_str(), pw_properties_new(
      PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, name.c_str(), PW_KEY_MEDIA_CLASS, "Audio/Sink",
      PW_KEY_NODE_GROUP, "pipewire.dummy", PW_KEY_NODE_VIRTUAL, "false",
      PW_KEY_PRIORITY_SESSION, output.index == 2 ? "2000" : "1000",
      "node.want-driver", "true", "node.pause-on-idle", "false",
      "audio.channels", "2", "audio.position", "[ FL FR ]",
      "node.device.profile.name", ((output.test->profile && output.test->stage == 3) ||
          (output.test->recovery && output.test->stage == 7) ||
          (output.test->inventory && output.test->stage == 2)) && output.index == 0 ? "changed" : "", nullptr));
  if (output.stream == nullptr) return false;
  output.listener = {};
  pw_stream_add_listener(output.stream, &output.listener, &events, &output);
  return connectStereo(output.stream, PW_DIRECTION_INPUT, PW_STREAM_FLAG_MAP_BUFFERS);
}

static void changeOutputGraph(void *data, std::uint64_t) {
  auto &test = *static_cast<AudioTest *>(data);
  if (test.recovery) {
    const auto index = test.stage == 2 || test.stage == 3 ? 1U :
        test.stage == 5 || test.stage == 6 ? 2U : 0U;
    if (test.stage != 11) {
      auto &output = test.outputs[index];
      if (output.stream != nullptr) {
        spa_hook_remove(&output.listener);
        pw_stream_destroy(output.stream);
        output.stream = nullptr;
        output.ready = false;
      }
      if (test.stage != 1 && test.stage != 2 && test.stage != 5 && test.stage != 9 &&
          !createTestOutput(output)) return fail(test, "cannot recover test output");
    }
    if (test.stage == 2) {
      test.awaitingControl = true;
      std::cerr << "product:recovery-stage-2\n" << std::flush;
    }
    return;
  }
  if (test.inventory) {
    auto &output = test.outputs[0];
    if (output.stream != nullptr) {
      spa_hook_remove(&output.listener);
      pw_stream_destroy(output.stream);
      output.stream = nullptr;
      output.ready = false;
    }
    if (test.stage > 1 && !createTestOutput(output)) fail(test, "cannot recreate inventory output");
    return;
  }
  if (test.latencyReconnect) {
    auto &output = test.outputs[1];
    if (test.stage == 1) {
      spa_hook_remove(&output.listener);
      pw_stream_destroy(output.stream);
      output.stream = nullptr;
      output.ready = false;
    } else if (!createTestOutput(output)) fail(test, "cannot restore the slower output");
    return;
  }
  if (test.latency) return outputParameterChanged(&test.outputs[1], SPA_PARAM_Latency, nullptr);
  if (test.restart) {
    // ALSA nodes are owned by WirePlumber and reappear with new generations
    // after a restart. Keep the application source alive while simulating it.
    for (auto &output : test.outputs) {
      spa_hook_remove(&output.listener);
      pw_stream_destroy(output.stream);
      output.stream = nullptr;
      output.ready = false;
      if (!createTestOutput(output)) return fail(test, "cannot restore test hardware");
    }
    test.restartResumed = true;
    return;
  }
  auto &output = test.outputs[0];
  if (output.stream != nullptr) {
    spa_hook_remove(&output.listener);
    pw_stream_destroy(output.stream);
    output.stream = nullptr;
    output.ready = false;
  }
  if (test.stage != 2 && !createTestOutput(output)) fail(test, "cannot reconnect test output");
}

static int runAudio(std::string_view scenario) {
  pw_init(nullptr, nullptr);
  auto test = AudioTest{};
  if (const auto *name = std::getenv("PIPETUNE_PRODUCT_INPUT")) test.inputName = name;
  test.matrix = scenario != "product-bypass";
  test.slots = scenario == "product-slots";
  test.daemonVolume = scenario.starts_with("product-daemon-volume");
  test.restoredVolume = test.daemonVolume && !scenario.ends_with("-save");
  test.savedMasterGain = scenario.find("-zero-") != std::string_view::npos ? 0.0F : 0.5F;
  test.volume = scenario == "product-volume" || test.daemonVolume;
  if (test.restoredVolume) test.stage = scenario.ends_with("-unmuted") ? 3 : 2;
  test.disconnected = scenario == "product-disconnected";
  test.profile = scenario == "product-profile";
  test.restartMuted = scenario == "product-restart-mute";
  test.restart = scenario == "product-restart" || test.restartMuted;
  test.reconnect = scenario == "product-reconnect" || test.profile || test.restart;
  test.intentionalDelay = scenario == "product-time-alignment";
  test.latencyReconnect = scenario == "product-latency-reconnect";
  test.latencyUnits = scenario == "product-latency-units";
  test.latency = scenario == "product-latency" || test.latencyUnits || test.intentionalDelay || test.latencyReconnect;
  test.graphClock = scenario == "product-output-clock" || scenario == "product-output-clock-alt";
  test.clockUnavailable = scenario == "product-output-clock-unavailable";
  test.clockReconnect = scenario == "product-reconnect";
  test.initialGraphRate = scenario.ends_with("-alt") ? 44100 : 48000;
  test.graphQuantum = scenario.ends_with("-alt") ? 512 : 256;
  test.controls = scenario == "product-controls" || test.graphClock;
  test.outputChange = scenario == "product-output-change";
  test.outputTimeout = scenario == "product-output-timeout";
  test.recovery = scenario == "product-output-recovery";
  test.inventory = scenario.starts_with("product-output-status");
  test.loop = pw_main_loop_new(nullptr);
  if (test.loop == nullptr) return 1;
  // Block and register the control signal before PipeWire creates data threads.
  auto *signal = pw_loop_add_signal(pw_main_loop_get_loop(test.loop), SIGUSR1, startSource, &test);
  auto *resume = pw_loop_add_signal(pw_main_loop_get_loop(test.loop), SIGUSR2, [](void *data, int) {
    auto &test = *static_cast<AudioTest *>(data);
    if (test.recovery) {
      if (!test.awaitingControl) return fail(test, "unexpected recovery transition");
      if (test.stage == 11) return static_cast<void>(pw_main_loop_quit(test.loop));
      test.awaitingControl = false;
      ++test.stage;
      test.recoveryStarted = std::chrono::steady_clock::now();
      for (auto &output : test.outputs) {
        output.frames = 0; output.matched = false;
        output.zeroFrames = 0; output.longestSilence = 0;
      }
      pw_loop_signal_event(pw_main_loop_get_loop(test.loop), test.graphEvent);
      return;
    }
    if (test.clockUnavailable || (test.graphClock && test.stage == 8)) {
      if (!test.awaitingControl) return fail(test, "unexpected final timing acknowledgement");
      pw_main_loop_quit(test.loop);
      return;
    }
    if (test.inventory && ++test.stage == 4) return static_cast<void>(pw_main_loop_quit(test.loop));
    if (test.controls || test.outputChange || test.outputTimeout) {
      if (!test.awaitingControl) return fail(test, "unexpected control transition");
      test.awaitingControl = false;
      ++test.stage;
      for (auto &output : test.outputs) { output.frames = 0; output.matched = false; }
      return;
    }
    pw_loop_signal_event(pw_main_loop_get_loop(test.loop), test.graphEvent);
  }, &test);
  if (signal == nullptr || resume == nullptr) return 1;
  test.context = pw_context_new(pw_main_loop_get_loop(test.loop), nullptr, 0);
  if (test.context == nullptr) return 1;
  test.core = pw_context_connect(test.context, nullptr, 0);
  if (test.core == nullptr) return 1;
  auto clockObserver = pipetune::PipeWireGraphClockPtr{};
  auto pathObserver = pipetune::PipeWireOutputPathsPtr{};
  if (test.latency || test.graphClock || test.clockUnavailable || test.clockReconnect) {
    pathObserver = pipetune::observePipeWireOutputPaths(test.core, test.inputName, [](const auto &paths, void *data) {
      static_cast<AudioTest *>(data)->paths = paths;
    }, &test);
    if (pathObserver == nullptr) return 1;
  }
  if (test.graphClock || test.clockUnavailable || test.clockReconnect) {
    clockObserver = pipetune::observePipeWireGraphClocks(test.core, [](const auto &clocks, void *data) {
      auto &test = *static_cast<AudioTest *>(data);
      test.clocks = clocks;
      test.clockSnapshot = true;
    }, &test);
    if (clockObserver == nullptr) return 1;
  }
  test.registry = pw_core_get_registry(test.core, PW_VERSION_REGISTRY, 0);
  static const auto registryEvents = [] {
    auto value = pw_registry_events{};
    value.version = PW_VERSION_REGISTRY_EVENTS;
    value.global = [](void *data, std::uint32_t id, std::uint32_t, const char *type, std::uint32_t version, const spa_dict *props) {
      auto &test = *static_cast<AudioTest *>(data);
      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == nullptr || test.control != nullptr) return;
      const auto *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (name != nullptr && std::string_view(name) == test.inputName) {
        test.publicInputId = id;
        test.control = static_cast<pw_node *>(pw_registry_bind(test.registry, id, type, std::min<std::uint32_t>(version, PW_VERSION_NODE), 0));
        if (test.control == nullptr) return fail(test, "cannot observe master controls");
        static const auto controlEvents = [] {
          auto value = pw_node_events{}; value.version = PW_VERSION_NODE_EVENTS;
          value.param = [](void *data, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod *param) {
            auto &test = *static_cast<AudioTest *>(data);
            if (id != SPA_PARAM_Props || param == nullptr) return;
            if (const auto *prop = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes)) {
              auto values = std::array<float, 2>{};
              if (spa_pod_copy_array(&prop->value, SPA_TYPE_Float, values.data(), values.size()) == 2)
                test.visibleGain = values[0] == values[1] ? values[0] : -1.0F;
            }
            if (const auto *prop = spa_pod_find_prop(param, nullptr, SPA_PROP_mute))
              spa_pod_get_bool(&prop->value, &test.visibleMute);
          };
          return value;
        }();
        test.controlListener = {};
        pw_node_add_listener(test.control, &test.controlListener, &controlEvents, &test);
        auto parameter = std::uint32_t{SPA_PARAM_Props};
        if (pw_node_subscribe_params(test.control, &parameter, 1) < 0) fail(test, "cannot subscribe to master controls");
      }
    };
    value.global_remove = [](void *data, std::uint32_t id) {
      auto &test = *static_cast<AudioTest *>(data);
      if (id == test.publicInputId) {
        if ((test.reconnect && !test.restart) || (test.recovery && test.stage < 9))
          fail(test, "public input disappeared during device reconnection");
        if (test.control != nullptr) {
          spa_hook_remove(&test.controlListener);
          pw_proxy_destroy(reinterpret_cast<pw_proxy *>(test.control));
        }
        test.visibleGain = -1.0F;
        test.control = nullptr;
        test.publicInputId = PW_ID_ANY;
      }
    };
    return value;
  }();
  if (test.registry == nullptr) return 1;
  pw_registry_add_listener(test.registry, &test.registryListener, &registryEvents, &test);
  for (auto index = 0U; index < test.outputs.size(); ++index) {
    auto &output = test.outputs[index]; output.test = &test; output.index = index;
    if (!createTestOutput(output)) return 1;
  }
  test.graphEvent = pw_loop_add_event(pw_main_loop_get_loop(test.loop), changeOutputGraph, &test);
  if (test.graphEvent == nullptr) return 1;
  auto *timer = pw_loop_add_timer(pw_main_loop_get_loop(test.loop), [](void *data, std::uint64_t) {
    fail(*static_cast<AudioTest *>(data), "product PCM test timed out");
  }, &test);
  auto deadline = timespec{.tv_sec = test.outputTimeout || test.recovery ? 30 : 15, .tv_nsec = 0}; auto interval = timespec{};
  if (timer == nullptr || signal == nullptr ||
      pw_loop_update_timer(pw_main_loop_get_loop(test.loop), timer, &deadline, &interval, false) < 0) return 1;
  pw_main_loop_run(test.loop);
  if (test.source != nullptr) pw_stream_destroy(test.source);
  for (auto &output : test.outputs) if (output.stream != nullptr) pw_stream_destroy(output.stream);
  if (test.control != nullptr) {
    spa_hook_remove(&test.controlListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(test.control));
  }
  spa_hook_remove(&test.registryListener);
  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(test.registry));
  pw_loop_destroy_source(pw_main_loop_get_loop(test.loop), test.graphEvent);
  pw_loop_destroy_source(pw_main_loop_get_loop(test.loop), timer);
  pw_loop_destroy_source(pw_main_loop_get_loop(test.loop), signal);
  pw_loop_destroy_source(pw_main_loop_get_loop(test.loop), resume);
  clockObserver.reset();
  pathObserver.reset();
  pw_core_disconnect(test.core); pw_context_destroy(test.context); pw_main_loop_destroy(test.loop); pw_deinit();
  if (!test.error.empty()) std::cerr << test.error << '\n';
  std::cout << "{\"receivedFrames\":[" << test.outputs[0].frames << ',' << test.outputs[1].frames
            << "],\"producedFrames\":" << test.produced << ",\"stages\":[";
  for (auto stage = 0U; stage <= test.stage; ++stage) {
    if (stage != 0) std::cout << ',';
    std::cout << '[' << test.stageFrames[stage][0] << ',' << test.stageFrames[stage][1] << ']';
  }
  std::cout << "],\"singleModeFrames\":" << test.singleModeFrames << ",\"compensationFrames\":[" << test.compensation[0] << ',' << test.compensation[1] << ',' << test.compensation[2] << ']';
  if (test.graphClock) {
    std::cout << ",\"graphRates\":[";
    for (auto stage = 0U; stage < test.observedGraphRates.size(); ++stage) {
      if (stage != 0) std::cout << ',';
      std::cout << test.observedGraphRates[stage];
    }
    std::cout << "],\"graphQuanta\":[";
    for (auto stage = 0U; stage < test.observedGraphQuanta.size(); ++stage) {
      if (stage != 0) std::cout << ',';
      std::cout << test.observedGraphQuanta[stage];
    }
    std::cout << ']';
  }
  if (test.clockReconnect) std::cout << ",\"clockGenerations\":[" << test.clockGenerations[0] << ',' << test.clockGenerations[1] << ']';
  if (test.recovery) {
    std::cout << ",\"recoveryMilliseconds\":[";
    for (auto stage = 0U; stage <= test.stage; ++stage) {
      if (stage != 0) std::cout << ',';
      std::cout << test.recoveryMilliseconds[stage];
    }
    std::cout << "],\"maximumSilenceFrames\":[";
    for (auto stage = 0U; stage <= test.stage; ++stage) {
      if (stage != 0) std::cout << ',';
      std::cout << '[' << test.recoverySilence[stage][0] << ',' << test.recoverySilence[stage][1] << ']';
    }
    std::cout << ']';
  }
  std::cout << "}\n";
  return test.error.empty() ? 0 : 1;
}

int main(int argc, char **argv) {
  const auto signals = pipetune::blockPipeWireTerminationSignals();
  if (!signals.empty()) { std::cerr << signals << '\n'; return 1; }
  if (argc < 3) return 2;
  if (std::string_view(argv[1]) == "control" && argc == 4) {
    const auto result = pipetune::exchangeControlMessage(argv[2], argv[3]);
    if (!result.error.empty()) { std::cerr << result.error << '\n'; return 1; }
    std::cout << result.response << '\n';
    return 0;
  }
  if (std::string_view(argv[1]) == "audio") return runAudio(argv[2]);
  if (std::string_view(argv[1]) == "runtime" && argc == 4)
    return runProduct(std::string_view(argv[2]) == "bypass" ? nullptr : argv[2], argv[3]);
  return 2;
}
