#include "pipetune/pipewire_pipeline.h"

#include "audio_bridge.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pipetune {

constexpr auto kSampleBytes = std::uint32_t{sizeof(float)};
constexpr auto kReadinessTimeoutSeconds = std::time_t{5};

struct PipeWireRuntime;

struct StreamCallbackContext {
  PipeWireRuntime *runtime;
  bool capture;
};

struct PipeWireRuntime {
  DspPipeline &pipeline;
  PipeWirePipelineOptions options;
  PipeWireRunMode mode;
  PlanarAudioRing ring;
  std::vector<float> captureScratch;
  std::vector<float> playbackScratch;
  std::atomic<std::uint64_t> processingErrors;
  std::uint64_t processedInputFrames;
  pw_main_loop *mainLoop;
  pw_stream *captureStream;
  pw_stream *playbackStream;
  spa_source *timeoutSource;
  spa_source *interruptSource;
  spa_source *terminateSource;
  pw_stream_events captureEvents;
  pw_stream_events playbackEvents;
  StreamCallbackContext captureContext;
  StreamCallbackContext playbackContext;
  bool captureReady;
  bool playbackReady;
  bool readyNotified;
  bool completed;
  std::string error;

  PipeWireRuntime(DspPipeline &preparedPipeline,
                  const PipeWirePipelineOptions &runtimeOptions,
                  PipeWireRunMode runtimeMode)
      : pipeline(preparedPipeline), options(runtimeOptions), mode(runtimeMode),
        ring(runtimeOptions.channelCount, runtimeOptions.ringCapacityFrames),
        captureScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                           runtimeOptions.maxFrames,
                       0.0F),
        playbackScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                            runtimeOptions.maxFrames,
                        0.0F),
        processingErrors(0), processedInputFrames(0), mainLoop(nullptr),
        captureStream(nullptr), playbackStream(nullptr), timeoutSource(nullptr),
        interruptSource(nullptr), terminateSource(nullptr), captureEvents{},
        playbackEvents{}, captureContext{this, true}, playbackContext{this, false},
        captureReady(false), playbackReady(false), readyNotified(false),
        completed(false), error() {}

  ~PipeWireRuntime() {
    if (captureStream != nullptr) {
      pw_stream_destroy(captureStream);
    }
    if (playbackStream != nullptr) {
      pw_stream_destroy(playbackStream);
    }
    if (mainLoop != nullptr) {
      auto *loop = pw_main_loop_get_loop(mainLoop);
      if (timeoutSource != nullptr) {
        pw_loop_destroy_source(loop, timeoutSource);
      }
      if (interruptSource != nullptr) {
        pw_loop_destroy_source(loop, interruptSource);
      }
      if (terminateSource != nullptr) {
        pw_loop_destroy_source(loop, terminateSource);
      }
      pw_main_loop_destroy(mainLoop);
    }
  }
};

struct PipeWireLibraryScope {
  PipeWireLibraryScope() {
    pw_init(nullptr, nullptr);
  }

  ~PipeWireLibraryScope() {
    pw_deinit();
  }
};

static PipeWireRunResult validationError(std::string message) {
  return {.success = false,
          .error = std::move(message),
          .overrunFrames = 0,
          .underrunFrames = 0,
          .processingErrors = 0};
}

static std::string systemError(std::string_view operation, int result) {
  const auto errorNumber = result < 0 ? -result : errno;
  return std::string(operation) + ": " + std::strerror(errorNumber);
}

static void failRuntime(PipeWireRuntime &runtime, std::string message) {
  if (!runtime.error.empty()) {
    return;
  }
  runtime.error = std::move(message);
  if (runtime.mainLoop != nullptr) {
    pw_main_loop_quit(runtime.mainLoop);
  }
}

static bool isReadyState(pw_stream_state state) noexcept {
  return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
}

static void finishReadinessCheck(PipeWireRuntime &runtime) {
  if (!runtime.captureReady || !runtime.playbackReady) {
    return;
  }
  if (!runtime.readyNotified) {
    runtime.readyNotified = true;
    if (runtime.options.readyCallback != nullptr) {
      runtime.options.readyCallback(runtime.options.readyUserData);
    }
  }
  if (runtime.mode == PipeWireRunMode::untilReady) {
    runtime.completed = true;
    pw_main_loop_quit(runtime.mainLoop);
  }
}

static void streamStateChanged(void *data, pw_stream_state,
                               pw_stream_state state, const char *error) {
  auto &context = *static_cast<StreamCallbackContext *>(data);
  auto &runtime = *context.runtime;
  if (state == PW_STREAM_STATE_ERROR) {
    const auto detail = error == nullptr ? std::string("unknown PipeWire stream error")
                                         : std::string(error);
    failRuntime(runtime, (context.capture ? "virtual sink: " : "playback stream: ") +
                             detail);
    return;
  }
  if (context.capture) {
    runtime.captureReady = isReadyState(state);
  } else {
    runtime.playbackReady = isReadyState(state);
  }
  finishReadinessCheck(runtime);
}

static spa_audio_info_raw makeRawFormat(const PipeWirePipelineOptions &options) {
  auto info = spa_audio_info_raw{};
  info.format = SPA_AUDIO_FORMAT_F32P;
  info.rate = options.sampleRate;
  info.channels = options.channelCount;

  switch (options.channelCount) {
  case 1:
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    break;
  case 2:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    break;
  case 3:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_FC;
    break;
  case 4:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_RL;
    info.position[3] = SPA_AUDIO_CHANNEL_RR;
    break;
  case 5:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_FC;
    info.position[3] = SPA_AUDIO_CHANNEL_RL;
    info.position[4] = SPA_AUDIO_CHANNEL_RR;
    break;
  case 6:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_FC;
    info.position[3] = SPA_AUDIO_CHANNEL_LFE;
    info.position[4] = SPA_AUDIO_CHANNEL_RL;
    info.position[5] = SPA_AUDIO_CHANNEL_RR;
    break;
  case 7:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_FC;
    info.position[3] = SPA_AUDIO_CHANNEL_LFE;
    info.position[4] = SPA_AUDIO_CHANNEL_RC;
    info.position[5] = SPA_AUDIO_CHANNEL_SL;
    info.position[6] = SPA_AUDIO_CHANNEL_SR;
    break;
  case 8:
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    info.position[2] = SPA_AUDIO_CHANNEL_FC;
    info.position[3] = SPA_AUDIO_CHANNEL_LFE;
    info.position[4] = SPA_AUDIO_CHANNEL_RL;
    info.position[5] = SPA_AUDIO_CHANNEL_RR;
    info.position[6] = SPA_AUDIO_CHANNEL_SL;
    info.position[7] = SPA_AUDIO_CHANNEL_SR;
    break;
  default:
    break;
  }
  return info;
}

static spa_pod *buildBufferParameter(spa_pod_builder &builder,
                                     const PipeWirePipelineOptions &options) {
  auto frame = spa_pod_frame{};
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers,
                              SPA_PARAM_Buffers);
  spa_pod_builder_add(
      &builder, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 32),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(static_cast<int>(options.channelCount)),
      SPA_PARAM_BUFFERS_size,
      SPA_POD_CHOICE_RANGE_Int(static_cast<int>(options.maxFrames * kSampleBytes),
                               static_cast<int>(32 * kSampleBytes),
                               static_cast<int>(options.maxFrames * kSampleBytes)),
      SPA_PARAM_BUFFERS_stride, SPA_POD_Int(static_cast<int>(kSampleBytes)),
      SPA_PARAM_BUFFERS_dataType,
      SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd) |
                               (1 << SPA_DATA_MemId)),
      0);
  return static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame));
}

static void streamParameterChanged(void *data, std::uint32_t id,
                                   const spa_pod *parameter) {
  if (id != SPA_PARAM_Format || parameter == nullptr) {
    return;
  }

  auto &context = *static_cast<StreamCallbackContext *>(data);
  auto &runtime = *context.runtime;
  auto negotiated = spa_audio_info_raw{};
  const auto parseResult = spa_format_audio_raw_parse(parameter, &negotiated);
  if (parseResult < 0 || negotiated.format != SPA_AUDIO_FORMAT_F32P ||
      negotiated.rate != runtime.options.sampleRate ||
      negotiated.channels != runtime.options.channelCount) {
    failRuntime(runtime, "PipeWire negotiated an unsupported audio format");
    return;
  }

  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const spa_pod *parameters[] = {buildBufferParameter(builder, runtime.options)};
  auto *stream = context.capture ? runtime.captureStream : runtime.playbackStream;
  const auto updateResult = pw_stream_update_params(stream, parameters, 1);
  if (updateResult < 0) {
    failRuntime(runtime, systemError("cannot configure PipeWire buffers", updateResult));
  }
}

static bool inspectCaptureBuffer(const spa_buffer &buffer, std::uint32_t channelCount,
                                 std::uint32_t &frameCount) noexcept {
  if (buffer.n_datas < channelCount || buffer.datas == nullptr) {
    return false;
  }
  frameCount = UINT32_MAX;
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto &plane = buffer.datas[channel];
    if (plane.data == nullptr || plane.chunk == nullptr ||
        plane.maxsize < kSampleBytes || plane.maxsize % kSampleBytes != 0 ||
        plane.chunk->stride != static_cast<std::int32_t>(kSampleBytes) ||
        plane.chunk->offset % kSampleBytes != 0) {
      return false;
    }
    const auto byteCount = std::min(plane.chunk->size, plane.maxsize);
    frameCount = std::min(frameCount, byteCount / kSampleBytes);
  }
  return frameCount != UINT32_MAX;
}

static void copyCapturePlane(const spa_data &plane, std::uint32_t sourceFrame,
                             std::span<float> destination) noexcept {
  if ((plane.chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0) {
    std::fill(destination.begin(), destination.end(), 0.0F);
    return;
  }
  const auto byteCount =
      static_cast<std::uint32_t>(destination.size_bytes());
  const auto sourceByte =
      (plane.chunk->offset + sourceFrame * kSampleBytes) % plane.maxsize;
  const auto firstBytes = std::min(byteCount, plane.maxsize - sourceByte);
  auto *source = static_cast<const std::uint8_t *>(plane.data);
  std::memcpy(destination.data(), source + sourceByte, firstBytes);
  std::memcpy(reinterpret_cast<std::uint8_t *>(destination.data()) + firstBytes,
              source, byteCount - firstBytes);
}

static void captureProcess(void *data) {
  auto &context = *static_cast<StreamCallbackContext *>(data);
  auto &runtime = *context.runtime;
  auto *pipeWireBuffer = pw_stream_dequeue_buffer(runtime.captureStream);
  if (pipeWireBuffer == nullptr || pipeWireBuffer->buffer == nullptr) {
    return;
  }

  auto &buffer = *pipeWireBuffer->buffer;
  auto frameCount = std::uint32_t{0};
  if (!inspectCaptureBuffer(buffer, runtime.options.channelCount, frameCount)) {
    runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    pipeWireBuffer->size = 0;
    pw_stream_queue_buffer(runtime.captureStream, pipeWireBuffer);
    return;
  }

  auto sourceFrame = std::uint32_t{0};
  while (sourceFrame < frameCount) {
    const auto blockFrames =
        std::min(runtime.options.maxFrames, frameCount - sourceFrame);
    auto scratch = std::span<float>(runtime.captureScratch)
                       .first(static_cast<std::size_t>(runtime.options.channelCount) *
                              blockFrames);
    for (auto channel = std::uint32_t{0}; channel < runtime.options.channelCount;
         ++channel) {
      auto channelScratch = scratch.subspan(
          static_cast<std::size_t>(channel) * blockFrames, blockFrames);
      copyCapturePlane(buffer.datas[channel], sourceFrame, channelScratch);
    }

    const auto timeSeconds =
        static_cast<double>(runtime.processedInputFrames) / runtime.options.sampleRate;
    if (runtime.pipeline.process(scratch, runtime.options.channelCount, blockFrames,
                                 timeSeconds) != ProcessStatus::ok) {
      runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    }
    runtime.ring.write(scratch, blockFrames);
    runtime.processedInputFrames += blockFrames;
    sourceFrame += blockFrames;
  }

  pipeWireBuffer->size = frameCount;
  pw_stream_queue_buffer(runtime.captureStream, pipeWireBuffer);
}

static bool inspectPlaybackBuffer(const spa_buffer &buffer, std::uint32_t channelCount,
                                  std::uint32_t &capacityFrames) noexcept {
  if (buffer.n_datas < channelCount || buffer.datas == nullptr) {
    return false;
  }
  capacityFrames = UINT32_MAX;
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto &plane = buffer.datas[channel];
    if (plane.data == nullptr || plane.chunk == nullptr ||
        plane.maxsize < kSampleBytes) {
      return false;
    }
    capacityFrames = std::min(capacityFrames, plane.maxsize / kSampleBytes);
  }
  return capacityFrames != UINT32_MAX;
}

static void clearPlaybackChunks(spa_buffer &buffer,
                                std::uint32_t channelCount) noexcept {
  const auto availableChannels = std::min(buffer.n_datas, channelCount);
  for (auto channel = std::uint32_t{0}; channel < availableChannels; ++channel) {
    auto &plane = buffer.datas[channel];
    if (plane.chunk != nullptr) {
      plane.chunk->offset = 0;
      plane.chunk->size = 0;
      plane.chunk->stride = static_cast<std::int32_t>(kSampleBytes);
      plane.chunk->flags = SPA_CHUNK_FLAG_EMPTY;
    }
  }
}

static void playbackProcess(void *data) {
  auto &context = *static_cast<StreamCallbackContext *>(data);
  auto &runtime = *context.runtime;
  auto *pipeWireBuffer = pw_stream_dequeue_buffer(runtime.playbackStream);
  if (pipeWireBuffer == nullptr || pipeWireBuffer->buffer == nullptr) {
    return;
  }

  auto &buffer = *pipeWireBuffer->buffer;
  auto capacityFrames = std::uint32_t{0};
  if (!inspectPlaybackBuffer(buffer, runtime.options.channelCount, capacityFrames)) {
    runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    clearPlaybackChunks(buffer, runtime.options.channelCount);
    pipeWireBuffer->size = 0;
    pw_stream_queue_buffer(runtime.playbackStream, pipeWireBuffer);
    return;
  }

  const auto suggestedFrames =
      pipeWireBuffer->requested == 0
          ? capacityFrames
          : static_cast<std::uint32_t>(
                std::min<std::uint64_t>(pipeWireBuffer->requested, capacityFrames));
  auto outputFrame = std::uint32_t{0};
  while (outputFrame < suggestedFrames) {
    const auto blockFrames =
        std::min(runtime.options.maxFrames, suggestedFrames - outputFrame);
    auto scratch = std::span<float>(runtime.playbackScratch)
                       .first(static_cast<std::size_t>(runtime.options.channelCount) *
                              blockFrames);
    runtime.ring.read(scratch, blockFrames);
    for (auto channel = std::uint32_t{0}; channel < runtime.options.channelCount;
         ++channel) {
      const auto source = scratch.subspan(
          static_cast<std::size_t>(channel) * blockFrames, blockFrames);
      auto *destination = static_cast<float *>(buffer.datas[channel].data);
      std::copy(source.begin(), source.end(), destination + outputFrame);
    }
    outputFrame += blockFrames;
  }

  for (auto channel = std::uint32_t{0}; channel < runtime.options.channelCount;
       ++channel) {
    auto &chunk = *buffer.datas[channel].chunk;
    chunk.offset = 0;
    chunk.size = suggestedFrames * kSampleBytes;
    chunk.stride = static_cast<std::int32_t>(kSampleBytes);
    chunk.flags = suggestedFrames == 0 ? SPA_CHUNK_FLAG_EMPTY : SPA_CHUNK_FLAG_NONE;
  }
  pipeWireBuffer->size = suggestedFrames;
  pw_stream_queue_buffer(runtime.playbackStream, pipeWireBuffer);
}

static void readinessTimedOut(void *data, std::uint64_t) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  failRuntime(runtime, "timed out while waiting for PipeWire stream negotiation");
}

static void interrupted(void *data, int) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  runtime.completed = true;
  pw_main_loop_quit(runtime.mainLoop);
}

static pw_properties *makeCommonProperties(const PipeWireRuntime &runtime,
                                           std::string_view nodeName) {
  auto *properties = pw_properties_new(nullptr, nullptr);
  if (properties == nullptr) {
    return nullptr;
  }
  const auto rate = std::to_string(runtime.options.sampleRate);
  const auto channels = std::to_string(runtime.options.channelCount);
  const auto nodeRate = "1/" + rate;
  pw_properties_set(properties, PW_KEY_APP_NAME, "PipeTune");
  pw_properties_set(properties, PW_KEY_MEDIA_TYPE, "Audio");
  pw_properties_set(properties, PW_KEY_MEDIA_ROLE, "DSP");
  pw_properties_set(properties, PW_KEY_NODE_NAME,
                    std::string(nodeName).c_str());
  pw_properties_set(properties, PW_KEY_NODE_GROUP,
                    (runtime.options.sinkName + ".group").c_str());
  pw_properties_set(properties, PW_KEY_NODE_LINK_GROUP,
                    (runtime.options.sinkName + ".link-group").c_str());
  pw_properties_set(properties, PW_KEY_NODE_RATE, nodeRate.c_str());
  pw_properties_set(properties, SPA_KEY_AUDIO_FORMAT, "F32P");
  pw_properties_set(properties, SPA_KEY_AUDIO_RATE, rate.c_str());
  pw_properties_set(properties, SPA_KEY_AUDIO_CHANNELS, channels.c_str());
  return properties;
}

static pw_properties *makeCaptureProperties(const PipeWireRuntime &runtime) {
  auto *properties = makeCommonProperties(runtime, runtime.options.sinkName);
  if (properties == nullptr) {
    return nullptr;
  }
  pw_properties_set(properties, PW_KEY_MEDIA_CLASS, "Audio/Sink");
  pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
  pw_properties_set(properties, PW_KEY_NODE_DESCRIPTION,
                    runtime.options.sinkDescription.c_str());
  pw_properties_set(properties, PW_KEY_NODE_VIRTUAL, "true");
  return properties;
}

static pw_properties *makePlaybackProperties(const PipeWireRuntime &runtime) {
  auto *properties =
      makeCommonProperties(runtime, runtime.options.sinkName + ".output");
  if (properties == nullptr) {
    return nullptr;
  }
  pw_properties_set(properties, PW_KEY_MEDIA_CLASS, "Stream/Output/Audio");
  pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
  pw_properties_set(properties, PW_KEY_NODE_PASSIVE, "true");
  if (!runtime.options.targetObject.empty()) {
    pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                      runtime.options.targetObject.c_str());
  }
  return properties;
}

static bool connectStream(PipeWireRuntime &runtime, pw_stream *stream,
                          pw_direction direction, bool autoconnect) {
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  auto info = makeRawFormat(runtime.options);
  const spa_pod *parameters[] = {
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)};
  auto flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (autoconnect) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  }
  const auto result =
      pw_stream_connect(stream, direction, PW_ID_ANY,
                        static_cast<pw_stream_flags>(flags), parameters, 1);
  if (result < 0) {
    failRuntime(runtime, systemError("cannot connect PipeWire stream", result));
    return false;
  }
  return true;
}

static bool createMainLoop(PipeWireRuntime &runtime) {
  runtime.mainLoop = pw_main_loop_new(nullptr);
  if (runtime.mainLoop == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire main loop", -errno));
    return false;
  }
  return true;
}

static bool createStreams(PipeWireRuntime &runtime) {
  runtime.captureEvents.version = PW_VERSION_STREAM_EVENTS;
  runtime.captureEvents.state_changed = streamStateChanged;
  runtime.captureEvents.param_changed = streamParameterChanged;
  runtime.captureEvents.process = captureProcess;
  runtime.playbackEvents.version = PW_VERSION_STREAM_EVENTS;
  runtime.playbackEvents.state_changed = streamStateChanged;
  runtime.playbackEvents.param_changed = streamParameterChanged;
  runtime.playbackEvents.process = playbackProcess;

  runtime.captureStream =
      pw_stream_new_simple(pw_main_loop_get_loop(runtime.mainLoop),
                           "PipeTune virtual sink", makeCaptureProperties(runtime),
                           &runtime.captureEvents, &runtime.captureContext);
  if (runtime.captureStream == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire virtual sink", -errno));
    return false;
  }

  runtime.playbackStream =
      pw_stream_new_simple(pw_main_loop_get_loop(runtime.mainLoop),
                           "PipeTune playback", makePlaybackProperties(runtime),
                           &runtime.playbackEvents, &runtime.playbackContext);
  if (runtime.playbackStream == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire playback stream", -errno));
    return false;
  }

  return connectStream(runtime, runtime.captureStream, PW_DIRECTION_INPUT, false) &&
         connectStream(runtime, runtime.playbackStream, PW_DIRECTION_OUTPUT, true);
}

static bool configureCompletionSources(PipeWireRuntime &runtime) {
  auto *loop = pw_main_loop_get_loop(runtime.mainLoop);
  if (runtime.mode == PipeWireRunMode::untilReady) {
    runtime.timeoutSource = pw_loop_add_timer(loop, readinessTimedOut, &runtime);
    if (runtime.timeoutSource == nullptr) {
      failRuntime(runtime, systemError("cannot create readiness timer", -errno));
      return false;
    }
    auto delay = timespec{.tv_sec = kReadinessTimeoutSeconds, .tv_nsec = 0};
    auto interval = timespec{.tv_sec = 0, .tv_nsec = 0};
    const auto result = pw_loop_update_timer(
        loop, runtime.timeoutSource, &delay, &interval, false);
    if (result < 0) {
      failRuntime(runtime, systemError("cannot arm readiness timer", result));
      return false;
    }
    return true;
  }

  runtime.interruptSource = pw_loop_add_signal(loop, SIGINT, interrupted, &runtime);
  runtime.terminateSource = pw_loop_add_signal(loop, SIGTERM, interrupted, &runtime);
  if (runtime.interruptSource == nullptr || runtime.terminateSource == nullptr) {
    failRuntime(runtime, systemError("cannot install PipeWire signal handlers", -errno));
    return false;
  }
  return true;
}

static std::string validateOptions(const DspPipeline &pipeline,
                                   const PipeWirePipelineOptions &options) {
  if (options.sinkName.empty() || options.sinkName.find('\0') != std::string::npos) {
    return "PipeWire sink name must not be empty or contain NUL";
  }
  if (options.sinkDescription.empty() ||
      options.sinkDescription.find('\0') != std::string::npos) {
    return "PipeWire sink description must not be empty or contain NUL";
  }
  if (options.targetObject.find('\0') != std::string::npos) {
    return "PipeWire target object must not contain NUL";
  }
  if (options.targetObject == options.sinkName) {
    return "PipeWire playback target must not be the PipeTune virtual sink";
  }
  if (options.sampleRate < 32000 || options.sampleRate > 192000) {
    return "PipeWire sample rate must be between 32000 and 192000 Hz";
  }
  if (options.channelCount == 0 || options.channelCount > 8) {
    return "PipeWire channel count must be between one and eight";
  }
  if (options.maxFrames < 32 ||
      options.maxFrames > static_cast<std::uint32_t>(INT_MAX) / kSampleBytes) {
    return "PipeWire maximum frame count is outside its supported range";
  }
  if (options.ringCapacityFrames < options.maxFrames) {
    return "PipeWire ring capacity must be at least the maximum frame count";
  }
  if (pipeline.sampleRate() != static_cast<float>(options.sampleRate) ||
      pipeline.maxChannels() < options.channelCount ||
      pipeline.maxFrames() < options.maxFrames) {
    return "DSP pipeline format does not cover the requested PipeWire format";
  }
  return {};
}

PipeWireRunResult runPipeWirePipeline(DspPipeline &pipeline,
                                      const PipeWirePipelineOptions &options,
                                      PipeWireRunMode mode) {
  const auto validation = validateOptions(pipeline, options);
  if (!validation.empty()) {
    return validationError(validation);
  }

  try {
    auto library = PipeWireLibraryScope{};
    auto runtime = PipeWireRuntime(pipeline, options, mode);
    if (createMainLoop(runtime) && configureCompletionSources(runtime) &&
        createStreams(runtime)) {
      const auto runResult = pw_main_loop_run(runtime.mainLoop);
      if (runResult < 0 && runtime.error.empty()) {
        failRuntime(runtime, systemError("PipeWire main loop failed", runResult));
      } else if (!runtime.completed && runtime.error.empty()) {
        failRuntime(runtime, "PipeWire main loop stopped before completion");
      }
    }
    return {.success = runtime.completed && runtime.error.empty(),
            .error = runtime.error,
            .overrunFrames = runtime.ring.overrunFrames(),
            .underrunFrames = runtime.ring.underrunFrames(),
            .processingErrors =
                runtime.processingErrors.load(std::memory_order_relaxed)};
  } catch (const std::exception &error) {
    return validationError(std::string("cannot prepare PipeWire pipeline: ") +
                           error.what());
  }
}

} // namespace pipetune
