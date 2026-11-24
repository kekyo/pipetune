/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_direct_output.h"
#include "pipewire_buffer_io.h"
#include "pipewire_stream_flags.h"

#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/audio/type-info.h>
#include <spa/param/latency-utils.h>
#include <spa/utils/json.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

namespace pipetune {

struct PipeWireDirectOutput {
  pw_stream *input = nullptr;
  pw_stream *output = nullptr;
  spa_hook inputListener = {};
  spa_hook outputListener = {};
  PipeWireDirectControl control = nullptr;
  PipeWireDirectError error = nullptr;
  void *data = nullptr;
  std::uint32_t channels = 0;
  spa_audio_info_raw outputFormat = {};
  std::atomic<bool> playbackAttached = false;
  std::uint64_t targetSerial = 0;
};

void PipeWireDirectOutputDeleter::operator()(PipeWireDirectOutput *pair) const noexcept {
  if (pair == nullptr) return;
  if (pair->input != nullptr) {
    pw_stream_set_active(pair->input, false);
    spa_hook_remove(&pair->inputListener);
    pw_stream_destroy(pair->input);
  }
  if (pair->output != nullptr) {
    spa_hook_remove(&pair->outputListener);
    pw_stream_destroy(pair->output);
  }
  delete pair;
}

pw_stream *pipeWireDirectInput(PipeWireDirectOutput &output) noexcept { return output.input; }

static void processDirect(void *data) {
  auto &pair = *static_cast<PipeWireDirectOutput *>(data);
  if (!pair.playbackAttached.load(std::memory_order_acquire)) return;
  // Capture only schedules this callback. It never returns buffers itself,
  // so ownership stays on this data loop until copying and recycling finish.
  auto *input = static_cast<pw_buffer *>(nullptr);
  while (auto *next = pw_stream_dequeue_buffer(pair.input)) {
    if (input != nullptr) {
      if (input->buffer) retirePipeWireCaptureBuffer(*input->buffer);
      pw_stream_queue_buffer(pair.input, input);
    }
    input = next;
  }
  auto *output = pw_stream_dequeue_buffer(pair.output);
  auto frames = std::uint32_t{0};
  const auto valid = input && input->buffer &&
      inspectPipeWireCaptureBuffer(*input->buffer, pair.channels, frames);
  if (output && output->buffer && output->buffer->n_datas == pair.channels) {
    auto capacity = UINT32_MAX;
    for (auto channel = 0U; channel < pair.channels; ++channel) {
      const auto &plane = output->buffer->datas[channel];
      capacity = std::min<std::uint32_t>(capacity, plane.data && plane.chunk ? plane.maxsize / sizeof(float) : 0U);
    }
    const auto count = valid ? std::min(frames, capacity) :
        static_cast<std::uint32_t>(std::min<std::uint64_t>(capacity, output->requested));
    for (auto channel = 0U; channel < pair.channels; ++channel) {
      auto &destination = output->buffer->datas[channel];
      if (!destination.chunk || !destination.data) continue;
      auto *samples = static_cast<float *>(destination.data);
      std::fill_n(samples, count, 0.0F);
      if (valid) {
        const auto &source = input->buffer->datas[channel];
        if ((source.chunk->flags & SPA_CHUNK_FLAG_EMPTY) == 0) {
          const auto offset = source.chunk->offset % source.maxsize;
          const auto bytes = count * sizeof(float);
          const auto first = std::min<std::size_t>(bytes, source.maxsize - offset);
          std::memcpy(samples, static_cast<const char *>(source.data) + offset, first);
          std::memcpy(reinterpret_cast<char *>(samples) + first, source.data, bytes - first);
        }
      }
      destination.chunk->offset = 0;
      destination.chunk->size = count * sizeof(float);
      destination.chunk->stride = sizeof(float);
      destination.chunk->flags = 0;
    }
    output->size = count;
  }
  if (input) {
    if (input->buffer) retirePipeWireCaptureBuffer(*input->buffer);
    pw_stream_queue_buffer(pair.input, input);
  }
  if (output) pw_stream_queue_buffer(pair.output, output);
}

static void discardDirectCapture(PipeWireDirectOutput &pair) {
  while (auto *buffer = pw_stream_dequeue_buffer(pair.input)) {
    if (buffer->buffer) retirePipeWireCaptureBuffer(*buffer->buffer);
    pw_stream_queue_buffer(pair.input, buffer);
  }
}

static void forwardLatency(pw_stream *other, const spa_pod *parameter) {
  auto latency = spa_latency_info{};
  if (parameter == nullptr || spa_latency_parse(parameter, &latency) < 0) return;
  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const spa_pod *params[] = {spa_latency_build(&builder, SPA_PARAM_Latency, &latency)};
  pw_stream_update_params(other, params, 1);
}

static void directState(void *data, pw_stream_state old, pw_stream_state state,
                        const char *error, bool playback) {
  auto &pair = *static_cast<PipeWireDirectOutput *>(data);
  if (pipeWireStateTransitionInvalidatesQueuedAudio(old, state)) {
    pw_stream_flush(pair.input, false);
    if (pair.playbackAttached.load(std::memory_order_acquire))
      pw_stream_flush(pair.output, false);
  }
  if (playback && !pair.playbackAttached.load(std::memory_order_acquire)) return;
  if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED)
    pair.error(std::string("Direct output: ") + (error ? error : "stream disconnected"), pair.data);
}

static void directInputState(void *data, pw_stream_state old, pw_stream_state state,
                             const char *error) {
  directState(data, old, state, error, false);
}

static void directPlaybackState(void *data, pw_stream_state old, pw_stream_state state,
                                const char *error) {
  directState(data, old, state, error, true);
}

static bool directFormat(pw_properties *props, spa_audio_info_raw &info) {
  info.format = SPA_AUDIO_FORMAT_F32P;
  info.rate = pw_properties_get_uint32(props, PW_KEY_AUDIO_RATE, 0);
  const auto *positions = pw_properties_get(props, SPA_KEY_AUDIO_POSITION);
  if (!positions) return false;
  auto json = spa_json{};
  auto array = spa_json{};
  spa_json_init(&json, positions, std::strlen(positions));
  if (spa_json_enter_array(&json, &array) <= 0) return false;
  auto name = std::array<char, 32>{};
  while (spa_json_get_string(&array, name.data(), name.size()) > 0) {
    if (info.channels == 16) return false;
    const auto position = spa_debug_type_find_type_short(spa_type_audio_channel, name.data());
    if (position == SPA_ID_INVALID) return false;
    info.position[info.channels++] = position;
  }
  return info.channels >= 2;
}

PipeWireDirectOutputPtr createPipeWireDirectOutput(pw_core *core, const std::string &arguments,
    PipeWireDirectControl control, PipeWireDirectError error, void *data,
    std::uint64_t targetSerial) {
  using Properties = std::unique_ptr<pw_properties, decltype(&pw_properties_free)>;
  auto all = Properties(pw_properties_new_string(arguments.c_str()), pw_properties_free);
  if (!all || !control || !error) return {};
  const auto *capture = pw_properties_get(all.get(), "capture.props");
  const auto *playback = pw_properties_get(all.get(), "playback.props");
  if (!capture || !playback) return {};
  auto inputProps = Properties(pw_properties_new_string(capture), pw_properties_free);
  auto outputProps = Properties(pw_properties_new_string(playback), pw_properties_free);
  if (!inputProps || !outputProps) return {};
  auto inputFormat = spa_audio_info_raw{}, outputFormat = spa_audio_info_raw{};
  if (!directFormat(inputProps.get(), inputFormat) || !directFormat(outputProps.get(), outputFormat) ||
      inputFormat.channels != outputFormat.channels) return {};
  if (targetSerial != 0)
    pw_properties_set(outputProps.get(), PW_KEY_TARGET_OBJECT, std::to_string(targetSerial).c_str());
  const auto *name = pw_properties_get(inputProps.get(), PW_KEY_NODE_NAME);
  if (!name) return {};
  const auto group = std::string(name) + ".group";
  for (auto *props : {inputProps.get(), outputProps.get()}) {
    pw_properties_set(props, PW_KEY_NODE_DESCRIPTION, "PipeTune Direct Output");
    pw_properties_set(props, PW_KEY_NODE_GROUP, group.c_str());
    pw_properties_set(props, PW_KEY_NODE_LINK_GROUP, group.c_str());
    pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
    pw_properties_set(props, "resample.prefill", "true");
  }
  pw_properties_set(outputProps.get(), "channelmix.lock-volumes", "true");
  auto pair = PipeWireDirectOutputPtr(new PipeWireDirectOutput());
  pair->control = control; pair->error = error; pair->data = data; pair->channels = inputFormat.channels;
  pair->outputFormat = outputFormat;
  pair->targetSerial = targetSerial;
  pair->input = pw_stream_new(core, "PipeTune direct input", inputProps.release());
  pair->output = pw_stream_new(core, "PipeTune direct output", outputProps.release());
  if (!pair->input || !pair->output) return {};
  static const auto inputEvents = [] {
    auto events = pw_stream_events{}; events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = directInputState;
    events.process = [](void *data) {
      auto &pair = *static_cast<PipeWireDirectOutput *>(data);
      if (pair.playbackAttached.load(std::memory_order_acquire))
        pw_stream_trigger_process(pair.output);
      else discardDirectCapture(pair);
    };
    events.param_changed = [](void *data, std::uint32_t id, const spa_pod *param) {
      auto &pair = *static_cast<PipeWireDirectOutput *>(data);
      if (id == SPA_PARAM_Props && pw_stream_get_node_id(pair.input) != PW_ID_ANY)
        pair.control(pair.input, param, pair.data);
      else if (id == SPA_PARAM_Latency && pair.playbackAttached.load(std::memory_order_acquire))
        forwardLatency(pair.output, param);
    };
    return events;
  }();
  static const auto outputEvents = [] {
    auto events = pw_stream_events{}; events.version = PW_VERSION_STREAM_EVENTS;
    events.state_changed = directPlaybackState; events.process = processDirect;
    events.param_changed = [](void *data, std::uint32_t id, const spa_pod *param) {
      if (id == SPA_PARAM_Latency) forwardLatency(static_cast<PipeWireDirectOutput *>(data)->input, param);
    };
    return events;
  }();
  pw_stream_add_listener(pair->input, &pair->inputListener, &inputEvents, pair.get());
  pw_stream_add_listener(pair->output, &pair->outputListener, &outputEvents, pair.get());
  // Playback must be ready before capture can trigger it. Both streams are
  // grouped on the same graph clock and transfer each channel by index.
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const spa_pod *parameters[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &outputFormat)};
  if (pw_stream_connect(pair->output, PW_DIRECTION_OUTPUT, PW_ID_ANY,
      makePipeWireStreamFlags(PW_DIRECTION_OUTPUT, true, targetSerial == 0), parameters, 1) < 0) return {};
  pair->playbackAttached.store(true, std::memory_order_release);
  builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  parameters[0] = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &inputFormat);
  if (pw_stream_connect(pair->input, PW_DIRECTION_INPUT, PW_ID_ANY,
      makePipeWireStreamFlags(PW_DIRECTION_INPUT, true, true), parameters, 1) < 0) return {};
  return pair;
}

std::string detachPipeWireDirectPlayback(PipeWireDirectOutput &pair) {
  if (!pair.playbackAttached.exchange(false, std::memory_order_acq_rel)) return {};
  const auto result = pw_stream_disconnect(pair.output);
  if (result < 0) {
    pair.playbackAttached.store(true, std::memory_order_release);
    return "cannot detach Direct playback: " + std::string(std::strerror(-result));
  }
  pair.targetSerial = 0;
  return {};
}

std::string attachPipeWireDirectPlayback(PipeWireDirectOutput &pair,
                                         std::uint64_t targetSerial) {
  if (targetSerial == 0) return "Direct playback requires a distribution serial";
  if (pair.playbackAttached.load(std::memory_order_acquire))
    return pair.targetSerial == targetSerial ? std::string{} :
        "Direct playback must be detached before changing the distribution";
  const auto serial = std::to_string(targetSerial);
  const auto property = spa_dict_item{PW_KEY_TARGET_OBJECT, serial.c_str()};
  const auto properties = spa_dict{0, 1, &property};
  const auto update = pw_stream_update_properties(pair.output, &properties);
  if (update < 0)
    return "cannot select Direct playback distribution: " + std::string(std::strerror(-update));
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const spa_pod *parameters[] = {
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &pair.outputFormat)};
  const auto connect = pw_stream_connect(pair.output, PW_DIRECTION_OUTPUT, PW_ID_ANY,
      makePipeWireStreamFlags(PW_DIRECTION_OUTPUT, true, false), parameters, 1);
  if (connect < 0)
    return "cannot attach Direct playback: " + std::string(std::strerror(-connect));
  pair.targetSerial = targetSerial;
  pair.playbackAttached.store(true, std::memory_order_release);
  return {};
}

} // namespace pipetune
