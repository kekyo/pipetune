/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipewire/pipewire.h>
#include <pipewire/impl-module.h>
#include <spa/param/audio/format-utils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string>

struct Probe;

struct Endpoint {
  Probe *probe = nullptr;
  pw_stream *stream = nullptr;
  spa_hook listener = {};
  pw_stream_events events = {};
  std::uint32_t firstChannel = 0;
  std::uint64_t receivedFrames = 0;
  int previousTag = -1;
  bool selected = true;
};

struct Probe {
  pw_main_loop *loop = nullptr;
  pw_context *context = nullptr;
  pw_core *core = nullptr;
  pw_impl_module *combine = nullptr;
  pw_stream *source = nullptr;
  pw_stream_events sourceEvents = {};
  spa_hook sourceListener = {};
  spa_source *watchdog = nullptr;
  std::array<Endpoint, 3> endpoints = {};
  std::uint64_t producedFrames = 0;
  std::uint64_t channelErrors = 0;
  bool success = false;
  std::string error;
};

static void fail(Probe &probe, const std::string &message) {
  if (probe.error.empty()) probe.error = message;
  pw_main_loop_quit(probe.loop);
}

// A channel-specific offset identifies the channel, and a modulo-127 tag
// identifies consecutive frames without depending on startup silence length.
// Values are exactly representable in float32 and remain below 0.034 FS.
static float sampleValue(std::uint32_t channel, std::uint64_t frame) {
  return static_cast<float>((channel + 1u) * 512u + frame % 127u) /
         65536.0F;
}

static void capture(void *data) {
  auto &endpoint = *static_cast<Endpoint *>(data);
  auto &probe = *endpoint.probe;
  auto *queued = pw_stream_dequeue_buffer(endpoint.stream);
  if (queued == nullptr) return;
  auto *buffer = queued->buffer;
  if (buffer == nullptr || buffer->n_datas < 2) {
    pw_stream_queue_buffer(endpoint.stream, queued);
    fail(probe, "capture did not negotiate two planar channels");
    return;
  }
  auto frameCount = UINT32_MAX;
  for (auto channel = 0u; channel < 2; ++channel) {
    const auto &plane = buffer->datas[channel];
    if (plane.data == nullptr || plane.chunk == nullptr ||
        plane.chunk->stride != static_cast<int>(sizeof(float)) ||
        plane.chunk->offset > plane.maxsize ||
        plane.chunk->size > plane.maxsize - plane.chunk->offset) {
      pw_stream_queue_buffer(endpoint.stream, queued);
      fail(probe, "capture received an invalid planar buffer");
      return;
    }
    frameCount = std::min(frameCount, plane.chunk->size /
                                        static_cast<std::uint32_t>(sizeof(float)));
  }
  for (auto frame = 0u; frame < frameCount; ++frame) {
    auto samples = std::array<float, 2>{};
    for (auto channel = 0u; channel < 2; ++channel) {
      const auto &plane = buffer->datas[channel];
      std::memcpy(&samples[channel],
                  static_cast<const char *>(plane.data) + plane.chunk->offset +
                      frame * sizeof(float), sizeof(float));
    }
    if (samples[0] == 0.0F && samples[1] == 0.0F &&
        endpoint.receivedFrames == 0) continue;
    const auto tag = static_cast<int>(std::lround(samples[0] * 65536.0F)) -
                     static_cast<int>((endpoint.firstChannel + 1u) * 512u);
    const auto valid = endpoint.selected && tag >= 0 && tag < 127 &&
        samples[0] == sampleValue(endpoint.firstChannel, tag) &&
        samples[1] == sampleValue(endpoint.firstChannel + 1, tag) &&
        (endpoint.previousTag == -1 || tag == (endpoint.previousTag + 1) % 127);
    if (!valid) {
      ++probe.channelErrors;
      pw_stream_queue_buffer(endpoint.stream, queued);
      fail(probe, "received PCM does not preserve channel identity and sample order: channel " +
                      std::to_string(endpoint.firstChannel + 1) + " after " +
                      std::to_string(endpoint.receivedFrames) + " frames, values " +
                      std::to_string(samples[0]) + ", " + std::to_string(samples[1]) +
                      ", previous tag " + std::to_string(endpoint.previousTag));
      return;
    }
    endpoint.previousTag = tag;
    ++endpoint.receivedFrames;
  }
  pw_stream_queue_buffer(endpoint.stream, queued);
  if (probe.endpoints[0].receivedFrames >= 65536 &&
      probe.endpoints[1].receivedFrames >= 65536) {
    probe.success = true;
    pw_main_loop_quit(probe.loop);
  }
}

static void produce(void *data) {
  auto &probe = *static_cast<Probe *>(data);
  auto *queued = pw_stream_dequeue_buffer(probe.source);
  if (queued == nullptr) return;
  auto *buffer = queued->buffer;
  if (buffer == nullptr || buffer->n_datas < 4) {
    pw_stream_queue_buffer(probe.source, queued);
    fail(probe, "producer did not negotiate four planar channels");
    return;
  }
  auto frameCount = static_cast<std::uint32_t>(
      queued->requested == 0 ? 256 : queued->requested);
  for (auto channel = 0u; channel < 4; ++channel) {
    if (buffer->datas[channel].data == nullptr ||
        buffer->datas[channel].chunk == nullptr) {
      pw_stream_queue_buffer(probe.source, queued);
      fail(probe, "producer received an unmapped buffer");
      return;
    }
    frameCount = std::min(frameCount, buffer->datas[channel].maxsize /
                                        static_cast<std::uint32_t>(sizeof(float)));
  }
  for (auto channel = 0u; channel < 4; ++channel) {
    auto &plane = buffer->datas[channel];
    auto *samples = static_cast<float *>(plane.data);
    for (auto frame = 0u; frame < frameCount; ++frame)
      samples[frame] = sampleValue(channel, probe.producedFrames + frame);
    plane.chunk->offset = 0;
    plane.chunk->stride = sizeof(float);
    plane.chunk->size = frameCount * sizeof(float);
  }
  queued->size = frameCount;
  probe.producedFrames += frameCount;
  pw_stream_queue_buffer(probe.source, queued);
  if (probe.producedFrames >= 480000 && !probe.success)
    fail(probe, "both selected outputs did not receive the required PCM frames");
}

static void sourceStateChanged(void *data, pw_stream_state,
                               pw_stream_state state, const char *error) {
  if (state == PW_STREAM_STATE_ERROR)
    fail(*static_cast<Probe *>(data), error == nullptr ? "source failed" : error);
}

static void sinkStateChanged(void *data, pw_stream_state,
                             pw_stream_state state, const char *error) {
  if (state == PW_STREAM_STATE_ERROR)
    fail(*static_cast<Endpoint *>(data)->probe,
         error == nullptr ? "sink failed" : error);
}

static bool connectStream(pw_stream *stream, bool input) {
  auto raw = spa_audio_info_raw{};
  raw.format = SPA_AUDIO_FORMAT_F32P;
  raw.rate = 48000;
  raw.channels = input ? 2u : 4u;
  for (auto channel = 0u; channel < raw.channels; ++channel) {
    if (input)
      raw.position[channel] = channel == 0 ? SPA_AUDIO_CHANNEL_FL : SPA_AUDIO_CHANNEL_FR;
    else
      raw.position[channel] = SPA_AUDIO_CHANNEL_AUX0 + channel;
  }
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto *format = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &raw);
  // Main-loop process callbacks keep failure reporting and termination out of
  // the real-time thread. This executable is a verification driver, not DSP.
  const auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
      (input ? 0 : PW_STREAM_FLAG_AUTOCONNECT));
  return pw_stream_connect(stream, input ? PW_DIRECTION_INPUT : PW_DIRECTION_OUTPUT,
                           PW_ID_ANY, flags, &format, 1) >= 0;
}

static bool prepare(Probe &probe) {
  probe.loop = pw_main_loop_new(nullptr);
  if (probe.loop == nullptr) return false;
  probe.context = pw_context_new(pw_main_loop_get_loop(probe.loop), nullptr, 0);
  if (probe.context == nullptr) return false;
  probe.core = pw_context_connect(probe.context, nullptr, 0);
  if (probe.core == nullptr) return false;
  for (auto index = 0u; index < probe.endpoints.size(); ++index) {
    auto &endpoint = probe.endpoints[index];
    endpoint.probe = &probe;
    endpoint.firstChannel = index * 2u;
    endpoint.selected = index < 2;
    const auto name = "pipetune_probe_device_" + std::to_string(index);
    endpoint.stream = pw_stream_new(probe.core, name.c_str(), pw_properties_new(
        PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION, name.c_str(),
        PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_NODE_VIRTUAL, "true", "node.want-driver", "true",
        "node.pause-on-idle", "false", "audio.channels", "2",
        "audio.position", "[ FL FR ]", nullptr));
    if (endpoint.stream == nullptr) return false;
    endpoint.events.version = PW_VERSION_STREAM_EVENTS;
    endpoint.events.state_changed = sinkStateChanged;
    endpoint.events.process = capture;
    pw_stream_add_listener(endpoint.stream, &endpoint.listener, &endpoint.events, &endpoint);
    if (!connectStream(endpoint.stream, true)) return false;
  }
  const auto *arguments = R"conf(
    combine.mode = sink
    node.name = pipetune_probe_combined
    node.description = "PipeTune multi-device output probe"
    combine.latency-compensate = true
    combine.props = {
      audio.position = [ AUX0 AUX1 AUX2 AUX3 ]
      node.virtual = true
    }
    stream.props = {
      stream.dont-remix = true
      node.dont-fallback = true
      node.dont-move = true
      media.role = PipeTune-Probe-Output
    }
    stream.rules = [
      { matches = [ { node.name = pipetune_probe_device_0 } ]
        actions = { create-stream = {
          combine.audio.position = [ AUX0 AUX1 ]
          audio.position = [ FL FR ]
        } } }
      { matches = [ { node.name = pipetune_probe_device_1 } ]
        actions = { create-stream = {
          combine.audio.position = [ AUX2 AUX3 ]
          audio.position = [ FL FR ]
        } } }
    ]
  )conf";
  probe.combine = pw_context_load_module(probe.context,
      "libpipewire-module-combine-stream", arguments, nullptr);
  if (probe.combine == nullptr) return false;
  probe.source = pw_stream_new(probe.core, "PipeTune probe signal", pw_properties_new(
      PW_KEY_NODE_NAME, "pipetune_probe_signal",
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "PipeTune-Probe",
      PW_KEY_TARGET_OBJECT, "pipetune_probe_combined",
      "node.dont-fallback", "true", "node.dont-move", "true",
      "stream.dont-remix", "true", nullptr));
  if (probe.source == nullptr) return false;
  probe.sourceEvents.version = PW_VERSION_STREAM_EVENTS;
  probe.sourceEvents.state_changed = sourceStateChanged;
  probe.sourceEvents.process = produce;
  pw_stream_add_listener(probe.source, &probe.sourceListener, &probe.sourceEvents, &probe);
  if (!connectStream(probe.source, false)) return false;
  probe.watchdog = pw_loop_add_timer(pw_main_loop_get_loop(probe.loop),
      [](void *data, std::uint64_t) {
        fail(*static_cast<Probe *>(data), "PipeWire graph did not complete before the watchdog");
      }, &probe);
  if (probe.watchdog == nullptr) return false;
  auto deadline = timespec{20, 0};
  return pw_loop_update_timer(pw_main_loop_get_loop(probe.loop), probe.watchdog,
                              &deadline, nullptr, false) >= 0;
}

static void destroy(Probe &probe) {
  if (probe.watchdog != nullptr)
    pw_loop_destroy_source(pw_main_loop_get_loop(probe.loop), probe.watchdog);
  if (probe.source != nullptr) pw_stream_destroy(probe.source);
  if (probe.combine != nullptr) pw_impl_module_destroy(probe.combine);
  for (auto &endpoint : probe.endpoints)
    if (endpoint.stream != nullptr) pw_stream_destroy(endpoint.stream);
  if (probe.core != nullptr) pw_core_disconnect(probe.core);
  if (probe.context != nullptr) pw_context_destroy(probe.context);
  if (probe.loop != nullptr) pw_main_loop_destroy(probe.loop);
}

int main(int argc, char **argv) {
  pw_init(&argc, &argv);
  auto probe = Probe{};
  if (prepare(probe)) pw_main_loop_run(probe.loop);
  else probe.error = "cannot prepare multi-device output probe";
  destroy(probe);
  if (!probe.error.empty()) std::cerr << probe.error << '\n';
  std::cout << "{\"success\":" << (probe.success ? "true" : "false")
            << ",\"channels\":4,\"producedFrames\":" << probe.producedFrames
            << ",\"receivedFrames\":[" << probe.endpoints[0].receivedFrames << ','
            << probe.endpoints[1].receivedFrames << ',' << probe.endpoints[2].receivedFrames
            << "],\"channelErrors\":" << probe.channelErrors << "}\n";
  pw_deinit();
  return probe.success && probe.error.empty() ? 0 : 1;
}
