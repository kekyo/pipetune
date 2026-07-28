#include "pipetune/pipewire_pipeline.h"

#include "audio_bridge.h"
#include "default_sink_restore.h"
#include "dsp_pipeline_slot.h"
#include "input_telemetry.h"
#include "output_device_tracker.h"
#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/pod/builder.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <charconv>
#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
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

enum class CoreSyncPurpose {
  enumeration,
  defaultActivation,
  defaultRelease,
  defaultRestoration
};

enum class DefaultSinkTransition {
  inactive,
  activating,
  active,
  releasing,
  restoring
};

struct PipeWireRuntime {
  DspPipelineSlot pipeline;
  PipeWirePipelineOptions options;
  PipeWireRunMode mode;
  OutputDeviceTracker deviceTracker;
  PlanarAudioRing ring;
  std::vector<float> captureScratch;
  std::vector<float> playbackScratch;
  std::atomic<std::uint64_t> processingErrors;
  std::atomic<bool> defaultSinkActive;
  InputTelemetry inputTelemetry;
  std::atomic<bool> inputFormatNegotiated;
  std::uint64_t processedInputFrames;
  ProcessingMode processingMode;
  std::string activePreset;
  std::string configurationError;
  std::mutex playbackTargetMutex;
  std::unique_ptr<ControlServer> controlServer;
  pw_main_loop *mainLoop;
  pw_stream *captureStream;
  pw_stream *playbackStream;
  pw_core *trackingCore;
  pw_registry *registry;
  pw_metadata *defaultMetadata;
  spa_source *timeoutSource;
  spa_source *interruptSource;
  spa_source *terminateSource;
  pw_stream_events captureEvents;
  pw_stream_events playbackEvents;
  pw_core_events coreEvents;
  pw_registry_events registryEvents;
  pw_metadata_events metadataEvents;
  spa_hook coreListener;
  spa_hook registryListener;
  spa_hook metadataListener;
  StreamCallbackContext captureContext;
  StreamCallbackContext playbackContext;
  std::uint32_t defaultMetadataId;
  int trackingSyncSequence;
  CoreSyncPurpose trackingSyncPurpose;
  DefaultSinkTransition defaultSinkTransition;
  bool trackingReady;
  bool shutdownRequested;
  std::string observedDefaultSink;
  std::string defaultMetadataValue;
  std::string playbackTarget;
  bool captureReady;
  bool playbackReady;
  bool readyNotified;
  bool completed;
  std::string error;

  PipeWireRuntime(std::unique_ptr<DspPipeline> preparedPipeline,
                  const PipeWirePipelineOptions &runtimeOptions,
                  PipeWireRunMode runtimeMode)
      : pipeline(std::move(preparedPipeline)), options(runtimeOptions),
        mode(runtimeMode),
        deviceTracker(runtimeOptions.sinkName, runtimeOptions.targetObject),
        ring(runtimeOptions.channelCount, runtimeOptions.ringCapacityFrames),
        captureScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                           runtimeOptions.maxFrames,
                       0.0F),
        playbackScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                            runtimeOptions.maxFrames,
                        0.0F),
        processingErrors(0), defaultSinkActive(false), inputTelemetry(),
        inputFormatNegotiated(false), processedInputFrames(0),
        processingMode(runtimeOptions.initialPresetPath.empty()
                           ? ProcessingMode::bypass
                           : ProcessingMode::preset),
        activePreset(runtimeOptions.initialPresetPath.string()),
        configurationError(runtimeOptions.initialConfigurationError),
        playbackTargetMutex(), controlServer(), mainLoop(nullptr),
        captureStream(nullptr), playbackStream(nullptr), trackingCore(nullptr),
        registry(nullptr), defaultMetadata(nullptr), timeoutSource(nullptr),
        interruptSource(nullptr), terminateSource(nullptr), captureEvents{},
        playbackEvents{}, coreEvents{}, registryEvents{}, metadataEvents{},
        coreListener{}, registryListener{}, metadataListener{},
        captureContext{this, true}, playbackContext{this, false},
        defaultMetadataId(PW_ID_ANY), trackingSyncSequence(0),
        trackingSyncPurpose(CoreSyncPurpose::enumeration),
        defaultSinkTransition(DefaultSinkTransition::inactive),
        trackingReady(false), shutdownRequested(false),
        observedDefaultSink(), defaultMetadataValue(), playbackTarget(),
        captureReady(false), playbackReady(false), readyNotified(false),
        completed(false), error() {}

  ~PipeWireRuntime() {
    controlServer.reset();
    if (playbackStream != nullptr) {
      pw_stream_destroy(playbackStream);
    }
    if (defaultMetadata != nullptr) {
      spa_hook_remove(&metadataListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(defaultMetadata));
    }
    if (registry != nullptr) {
      spa_hook_remove(&registryListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    }
    if (trackingCore != nullptr) {
      spa_hook_remove(&coreListener);
    }
    if (captureStream != nullptr) {
      pw_stream_destroy(captureStream);
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
          .processingErrors = 0,
          .selectedTarget = {}};
}

static std::string systemError(std::string_view operation, int result) {
  const auto errorNumber = result < 0 ? -result : errno;
  return std::string(operation) + ": " + std::strerror(errorNumber);
}

static std::int64_t currentMonotonicNanoseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

static std::uint64_t currentUnixMilliseconds() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
}

static void applyTrackedTarget(PipeWireRuntime &runtime);
static void finishReadinessCheck(PipeWireRuntime &runtime);
static void maybeActivateDefaultSink(PipeWireRuntime &runtime);
static void maybeReleaseDefaultSink(PipeWireRuntime &runtime);

static std::string currentPlaybackTarget(PipeWireRuntime &runtime) {
  auto lock = std::scoped_lock(runtime.playbackTargetMutex);
  return runtime.playbackTarget;
}

static void requestControlStatusUpdate(PipeWireRuntime &runtime) {
  publishControlStatus(runtime.controlServer.get());
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

static std::int32_t parsePriority(const char *value) noexcept {
  if (value == nullptr) {
    return 0;
  }
  auto priority = std::int32_t{0};
  const auto end = value + std::strlen(value);
  const auto result = std::from_chars(value, end, priority);
  return result.ec == std::errc{} && result.ptr == end ? priority : 0;
}

static bool parseBooleanProperty(const char *value) noexcept {
  return value != nullptr &&
         (std::string_view(value) == "true" || std::string_view(value) == "1");
}

static void requestTrackingSync(PipeWireRuntime &runtime,
                                CoreSyncPurpose purpose) {
  const auto sequence = pw_core_sync(runtime.trackingCore, PW_ID_CORE, 0);
  if (sequence < 0) {
    failRuntime(runtime, systemError("cannot synchronize PipeWire registry",
                                     sequence));
    return;
  }
  runtime.trackingSyncSequence = sequence;
  runtime.trackingSyncPurpose = purpose;
}

static void completeRuntime(PipeWireRuntime &runtime) {
  runtime.completed = true;
  pw_main_loop_quit(runtime.mainLoop);
}

static bool writeEffectiveDefaultSink(
    PipeWireRuntime &runtime, std::string_view target,
    DefaultSinkTransition transition, CoreSyncPurpose syncPurpose,
    std::string_view operation) {
  if (runtime.defaultMetadata == nullptr) {
    failRuntime(runtime, "PipeWire default metadata is unavailable");
    return false;
  }
  runtime.defaultMetadataValue =
      target.empty() ? std::string{} : makeDefaultSinkMetadataValue(target);
  if (!target.empty() && runtime.defaultMetadataValue.empty()) {
    failRuntime(runtime, "cannot encode PipeWire default sink metadata");
    return false;
  }

  runtime.defaultSinkTransition = transition;
  runtime.defaultSinkActive.store(false, std::memory_order_release);
  requestControlStatusUpdate(runtime);
  const auto result = pw_metadata_set_property(
      runtime.defaultMetadata, PW_ID_CORE, "default.audio.sink",
      target.empty() ? nullptr : "Spa:String:JSON",
      target.empty() ? nullptr : runtime.defaultMetadataValue.c_str());
  if (result < 0) {
    failRuntime(runtime, systemError(operation, result));
    return false;
  }
  requestTrackingSync(runtime, syncPurpose);
  return runtime.error.empty();
}

static void trackingCoreDone(void *data, std::uint32_t id, int sequence) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  if (id != PW_ID_CORE || sequence != runtime.trackingSyncSequence) {
    return;
  }
  if (runtime.trackingSyncPurpose == CoreSyncPurpose::defaultActivation) {
    if (runtime.observedDefaultSink != runtime.options.sinkName) {
      runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
      maybeActivateDefaultSink(runtime);
      return;
    }
    runtime.defaultSinkTransition = DefaultSinkTransition::active;
    if (currentPlaybackTarget(runtime).empty()) {
      maybeReleaseDefaultSink(runtime);
      return;
    }
    runtime.defaultSinkActive.store(true, std::memory_order_release);
    requestControlStatusUpdate(runtime);
    finishReadinessCheck(runtime);
    return;
  }
  if (runtime.trackingSyncPurpose == CoreSyncPurpose::defaultRelease) {
    runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
    runtime.defaultSinkActive.store(false, std::memory_order_release);
    requestControlStatusUpdate(runtime);
    maybeActivateDefaultSink(runtime);
    return;
  }
  if (runtime.trackingSyncPurpose == CoreSyncPurpose::defaultRestoration) {
    runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
    runtime.defaultSinkActive.store(false, std::memory_order_release);
    requestControlStatusUpdate(runtime);
    completeRuntime(runtime);
    return;
  }

  runtime.trackingReady = true;
  runtime.deviceTracker.commitSelection();
  applyTrackedTarget(runtime);
  if (runtime.options.manageDefaultSink &&
      runtime.defaultMetadata == nullptr) {
    failRuntime(runtime, "PipeWire default metadata is unavailable");
    return;
  }
  maybeActivateDefaultSink(runtime);
}

static void trackingCoreError(void *data, std::uint32_t, int, int result,
                              const char *message) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  const auto detail = message == nullptr ? systemError("PipeWire core error", result)
                                         : std::string(message);
  failRuntime(runtime, "PipeWire device tracking failed: " + detail);
}

static int defaultMetadataProperty(void *data, std::uint32_t subject,
                                   const char *key, const char *,
                                   const char *value) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  if (subject != PW_ID_CORE) {
    return 0;
  }
  if (key == nullptr) {
    runtime.observedDefaultSink.clear();
    if (runtime.options.manageDefaultSink && !runtime.shutdownRequested &&
        runtime.defaultSinkTransition == DefaultSinkTransition::active) {
      runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
      maybeActivateDefaultSink(runtime);
    }
    return 0;
  }
  if (std::string_view(key) != "default.audio.sink") {
    return 0;
  }
  runtime.observedDefaultSink = defaultSinkNameFromMetadata(value);
  if (runtime.deviceTracker.setDefaultTarget(runtime.observedDefaultSink) &&
      runtime.trackingReady) {
    applyTrackedTarget(runtime);
  }
  if (runtime.options.manageDefaultSink && !runtime.shutdownRequested &&
      runtime.defaultSinkTransition == DefaultSinkTransition::active &&
      runtime.observedDefaultSink != runtime.options.sinkName) {
    runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
    maybeActivateDefaultSink(runtime);
  }
  return 0;
}

static void registryGlobal(void *data, std::uint32_t id, std::uint32_t,
                           const char *type, std::uint32_t version,
                           const spa_dict *properties) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  if (type == nullptr || properties == nullptr) {
    return;
  }
  if (std::string_view(type) == PW_TYPE_INTERFACE_Node) {
    const auto *mediaClass = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    const auto *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    if (mediaClass == nullptr || std::string_view(mediaClass) != "Audio/Sink" ||
        name == nullptr) {
      return;
    }
    const auto *description =
        spa_dict_lookup(properties, PW_KEY_NODE_DESCRIPTION);
    if (description == nullptr || description[0] == '\0') {
      description = spa_dict_lookup(properties, PW_KEY_NODE_NICK);
    }
    if (description == nullptr || description[0] == '\0') {
      description = name;
    }
    const auto *priority = spa_dict_lookup(properties, PW_KEY_PRIORITY_SESSION);
    const auto *virtualNode = spa_dict_lookup(properties, PW_KEY_NODE_VIRTUAL);
    const auto changed = runtime.deviceTracker.updateDevice(
        {.id = id,
         .name = name,
         .description = description,
         .priority = parsePriority(priority),
         .virtualNode = parseBooleanProperty(virtualNode)});
    if (changed && runtime.trackingReady) {
      applyTrackedTarget(runtime);
    }
    return;
  }
  if (std::string_view(type) != PW_TYPE_INTERFACE_Metadata ||
      runtime.defaultMetadata != nullptr) {
    return;
  }
  const auto *metadataName =
      spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
  if (metadataName == nullptr || std::string_view(metadataName) != "default") {
    return;
  }

  runtime.defaultMetadata = static_cast<pw_metadata *>(pw_registry_bind(
      runtime.registry, id, type,
      std::min(version, static_cast<std::uint32_t>(PW_VERSION_METADATA)), 0));
  if (runtime.defaultMetadata == nullptr) {
    failRuntime(runtime, systemError("cannot bind PipeWire default metadata",
                                     -errno));
    return;
  }
  runtime.defaultMetadataId = id;
  runtime.metadataEvents.version = PW_VERSION_METADATA_EVENTS;
  runtime.metadataEvents.property = defaultMetadataProperty;
  const auto listenerResult =
      pw_metadata_add_listener(runtime.defaultMetadata, &runtime.metadataListener,
                               &runtime.metadataEvents, &runtime);
  if (listenerResult < 0) {
    failRuntime(runtime,
                systemError("cannot monitor PipeWire default metadata",
                            listenerResult));
    return;
  }
  requestTrackingSync(runtime, CoreSyncPurpose::enumeration);
}

static void registryGlobalRemoved(void *data, std::uint32_t id) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  auto changed = runtime.deviceTracker.removeDevice(id);
  if (id == runtime.defaultMetadataId && runtime.defaultMetadata != nullptr) {
    spa_hook_remove(&runtime.metadataListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.defaultMetadata));
    runtime.defaultMetadata = nullptr;
    runtime.defaultMetadataId = PW_ID_ANY;
    runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
    runtime.defaultSinkActive.store(false, std::memory_order_release);
    requestControlStatusUpdate(runtime);
    runtime.observedDefaultSink.clear();
    if (runtime.shutdownRequested) {
      completeRuntime(runtime);
      return;
    }
  }
  if (changed && runtime.trackingReady) {
    applyTrackedTarget(runtime);
  }
}

static bool isReadyState(pw_stream_state state) noexcept {
  return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
}

static void finishReadinessCheck(PipeWireRuntime &runtime) {
  if (!runtime.captureReady || !runtime.playbackReady ||
      !runtime.inputFormatNegotiated.load(std::memory_order_acquire)) {
    return;
  }
  maybeActivateDefaultSink(runtime);
  if (runtime.options.manageDefaultSink &&
      !runtime.defaultSinkActive.load(std::memory_order_acquire)) {
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
    if (!context.capture) {
      runtime.playbackReady = false;
      return;
    }
    const auto detail = error == nullptr ? std::string("unknown PipeWire stream error")
                                         : std::string(error);
    failRuntime(runtime, (context.capture ? "virtual sink: " : "playback stream: ") +
                             detail);
    return;
  }
  if (context.capture) {
    runtime.captureReady = isReadyState(state);
    runtime.inputFormatNegotiated.store(runtime.captureReady,
                                        std::memory_order_release);
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
  } else if (context.capture) {
    runtime.inputFormatNegotiated.store(true, std::memory_order_release);
    finishReadinessCheck(runtime);
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
  if (frameCount != 0) {
    recordInputFrames(runtime.inputTelemetry, frameCount,
                      currentMonotonicNanoseconds());
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
  if (runtime.shutdownRequested) {
    return;
  }
  runtime.shutdownRequested = true;
  if (!runtime.options.manageDefaultSink ||
      runtime.defaultSinkTransition == DefaultSinkTransition::inactive ||
      runtime.defaultMetadata == nullptr) {
    completeRuntime(runtime);
    return;
  }
  const auto target = currentPlaybackTarget(runtime);
  static_cast<void>(writeEffectiveDefaultSink(
      runtime, target, DefaultSinkTransition::restoring,
      CoreSyncPurpose::defaultRestoration,
      "cannot restore PipeWire default sink"));
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

static pw_properties *makePlaybackProperties(const PipeWireRuntime &runtime,
                                             std::string_view target) {
  auto *properties =
      makeCommonProperties(runtime, runtime.options.sinkName + ".output");
  if (properties == nullptr) {
    return nullptr;
  }
  pw_properties_set(properties, PW_KEY_MEDIA_CLASS, "Stream/Output/Audio");
  pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
  pw_properties_set(properties, PW_KEY_NODE_PASSIVE, "true");
  pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                    std::string(target).c_str());
  return properties;
}

static bool connectStream(PipeWireRuntime &runtime, pw_stream *stream,
                          pw_direction direction, bool autoconnect,
                          bool dontReconnect) {
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
  if (dontReconnect) {
    flags |= PW_STREAM_FLAG_DONT_RECONNECT;
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

static bool createPlaybackStream(PipeWireRuntime &runtime,
                                 std::string_view target) {
  auto *properties = makePlaybackProperties(runtime, target);
  if (properties == nullptr) {
    failRuntime(runtime, "cannot allocate PipeWire playback properties");
    return false;
  }
  runtime.playbackStream =
      pw_stream_new_simple(pw_main_loop_get_loop(runtime.mainLoop),
                           "PipeTune playback", properties,
                           &runtime.playbackEvents, &runtime.playbackContext);
  if (runtime.playbackStream == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire playback stream",
                                     -errno));
    return false;
  }
  if (!connectStream(runtime, runtime.playbackStream, PW_DIRECTION_OUTPUT, true,
                     true)) {
    pw_stream_destroy(runtime.playbackStream);
    runtime.playbackStream = nullptr;
    return false;
  }
  return true;
}

static void applyTrackedTarget(PipeWireRuntime &runtime) {
  if (!runtime.trackingReady) {
    return;
  }
  const auto target = std::string(runtime.deviceTracker.selectedTarget());
  const auto targetUnchanged = target == currentPlaybackTarget(runtime);
  if (targetUnchanged &&
      ((target.empty() && runtime.playbackStream == nullptr) ||
       (!target.empty() && runtime.playbackStream != nullptr))) {
    if (target.empty()) {
      maybeReleaseDefaultSink(runtime);
    }
    return;
  }
  if (runtime.playbackStream != nullptr) {
    pw_stream_destroy(runtime.playbackStream);
    runtime.playbackStream = nullptr;
  }
  runtime.playbackReady = false;
  static_cast<void>(runtime.ring.discardQueuedFrames());
  {
    auto lock = std::scoped_lock(runtime.playbackTargetMutex);
    runtime.playbackTarget = target;
  }
  requestControlStatusUpdate(runtime);
  if (!target.empty()) {
    createPlaybackStream(runtime, target);
  }
  maybeReleaseDefaultSink(runtime);
  maybeActivateDefaultSink(runtime);
}

static void maybeReleaseDefaultSink(PipeWireRuntime &runtime) {
  if (!runtime.options.manageDefaultSink || runtime.shutdownRequested ||
      !runtime.trackingReady || runtime.defaultMetadata == nullptr ||
      !currentPlaybackTarget(runtime).empty() ||
      runtime.defaultSinkTransition == DefaultSinkTransition::activating ||
      runtime.defaultSinkTransition == DefaultSinkTransition::releasing ||
      runtime.defaultSinkTransition == DefaultSinkTransition::restoring ||
      (runtime.defaultSinkTransition != DefaultSinkTransition::active &&
       runtime.observedDefaultSink != runtime.options.sinkName)) {
    return;
  }
  static_cast<void>(writeEffectiveDefaultSink(
      runtime, {}, DefaultSinkTransition::releasing,
      CoreSyncPurpose::defaultRelease,
      "cannot release PipeTune as the PipeWire default sink"));
}

static void maybeActivateDefaultSink(PipeWireRuntime &runtime) {
  if (!runtime.options.manageDefaultSink || runtime.shutdownRequested ||
      !runtime.trackingReady || !runtime.captureReady ||
      !runtime.playbackReady ||
      runtime.defaultSinkTransition == DefaultSinkTransition::activating ||
      runtime.defaultSinkTransition == DefaultSinkTransition::releasing ||
      runtime.defaultSinkTransition == DefaultSinkTransition::restoring ||
      runtime.defaultSinkTransition == DefaultSinkTransition::active ||
      currentPlaybackTarget(runtime).empty()) {
    return;
  }
  static_cast<void>(writeEffectiveDefaultSink(
      runtime, runtime.options.sinkName, DefaultSinkTransition::activating,
      CoreSyncPurpose::defaultActivation,
      "cannot make PipeTune the PipeWire default sink"));
}

static ControlRuntimeStatus controlStatus(PipeWireRuntime &runtime) {
  const auto input = snapshotInputTelemetry(
      runtime.inputTelemetry, currentMonotonicNanoseconds(),
      currentUnixMilliseconds());
  const auto inputFormatNegotiated =
      runtime.inputFormatNegotiated.load(std::memory_order_acquire);
  return {.processingMode = runtime.processingMode,
          .activePreset = runtime.activePreset,
          .configurationError = runtime.configurationError,
          .activePluginCount = runtime.pipeline.activePluginCount(),
          .selectedTarget = currentPlaybackTarget(runtime),
          .defaultSinkActive =
              runtime.defaultSinkActive.load(std::memory_order_acquire),
          .overrunFrames = runtime.ring.overrunFrames(),
          .underrunFrames = runtime.ring.underrunFrames(),
          .processingErrors =
              runtime.processingErrors.load(std::memory_order_relaxed),
          .inputSampleFormat =
              inputFormatNegotiated ? std::string("F32P") : std::string{},
          .inputSampleRate =
              inputFormatNegotiated ? runtime.options.sampleRate : 0,
          .inputChannelCount =
              inputFormatNegotiated ? runtime.options.channelCount : 0,
          .inputFramesReceived = input.framesReceived,
          .inputLastReceivedUnixMilliseconds =
              input.lastReceivedUnixMilliseconds};
}

static ControlMessageResult closeControlResponse(std::string response,
                                                 bool publishStatus) {
  return {.response = std::move(response),
          .connectionMode = ControlConnectionMode::close,
          .publishStatus = publishStatus};
}

static std::string provideControlStatus(void *userData) {
  auto &runtime = *static_cast<PipeWireRuntime *>(userData);
  return makeControlStatusEvent(controlStatus(runtime));
}

static ControlMessageResult handleControlRequest(std::string_view message,
                                                 void *userData) {
  auto &runtime = *static_cast<PipeWireRuntime *>(userData);
  const auto request = parseControlRequest(message);
  if (!request.error.empty()) {
    return closeControlResponse(makeControlErrorResponse(request.error),
                                false);
  }
  if (request.request.command == ControlCommand::subscribe) {
    return {.response = provideControlStatus(&runtime),
            .connectionMode = ControlConnectionMode::subscribe,
            .publishStatus = false};
  }

  auto warnings = std::vector<ControlWarning>{};
  if (request.request.command == ControlCommand::bypass) {
    auto created = createBypassDspPipeline(
        {.sampleRate = static_cast<float>(runtime.options.sampleRate),
         .maxChannels = runtime.options.channelCount,
         .maxFrames = runtime.options.maxFrames});
    if (created.pipeline == nullptr) {
      return closeControlResponse(makeControlErrorResponse(created.error),
                                  false);
    }
    runtime.pipeline.replace(std::move(created.pipeline));
    runtime.processingMode = ProcessingMode::bypass;
    runtime.activePreset.clear();
    runtime.configurationError.clear();
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), true);
  }
  if (request.request.command == ControlCommand::loadPreset) {
    auto loaded = loadDspPipeline(
        request.request.presetPath,
        {.sampleRate = static_cast<float>(runtime.options.sampleRate),
         .maxChannels = runtime.options.channelCount,
         .maxFrames = runtime.options.maxFrames});
    if (loaded.pipeline == nullptr) {
      return closeControlResponse(makeControlErrorResponse(loaded.error),
                                  false);
    }
    warnings.reserve(loaded.warnings.size());
    for (auto &warning : loaded.warnings) {
      warnings.push_back({.nodeIndex = warning.nodeIndex,
                          .pluginName = std::move(warning.pluginName),
                          .reason = std::move(warning.reason)});
    }
    runtime.pipeline.replace(std::move(loaded.pipeline));
    runtime.processingMode = ProcessingMode::preset;
    runtime.activePreset = request.request.presetPath.string();
    runtime.configurationError.clear();
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), true);
  }
  return closeControlResponse(
      makeControlSuccessResponse(controlStatus(runtime), warnings), false);
}

static bool createControlServer(PipeWireRuntime &runtime) {
  if (runtime.options.controlSocketPath.empty()) {
    return true;
  }
  const auto options = ControlServerOptions{
      .handler = handleControlRequest,
      .statusProvider = provideControlStatus,
      .userData = &runtime,
  };
  auto started =
      startControlServer(runtime.options.controlSocketPath, options);
  if (started.server == nullptr) {
    failRuntime(runtime,
                "cannot start PipeTune control server: " + started.error);
    return false;
  }
  runtime.controlServer = std::move(started.server);
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

  auto *captureProperties = makeCaptureProperties(runtime);
  if (captureProperties == nullptr) {
    failRuntime(runtime, "cannot allocate PipeWire virtual sink properties");
    return false;
  }
  runtime.captureStream = pw_stream_new_simple(
      pw_main_loop_get_loop(runtime.mainLoop), "PipeTune virtual sink",
      captureProperties, &runtime.captureEvents, &runtime.captureContext);
  if (runtime.captureStream == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire virtual sink", -errno));
    return false;
  }

  if (!connectStream(runtime, runtime.captureStream, PW_DIRECTION_INPUT, false,
                     false)) {
    return false;
  }

  runtime.trackingCore = pw_stream_get_core(runtime.captureStream);
  if (runtime.trackingCore == nullptr) {
    failRuntime(runtime, "cannot access PipeWire core for device tracking");
    return false;
  }
  runtime.coreEvents.version = PW_VERSION_CORE_EVENTS;
  runtime.coreEvents.done = trackingCoreDone;
  runtime.coreEvents.error = trackingCoreError;
  const auto coreListenerResult =
      pw_core_add_listener(runtime.trackingCore, &runtime.coreListener,
                           &runtime.coreEvents, &runtime);
  if (coreListenerResult < 0) {
    failRuntime(runtime,
                systemError("cannot monitor PipeWire core", coreListenerResult));
    return false;
  }

  runtime.registry =
      pw_core_get_registry(runtime.trackingCore, PW_VERSION_REGISTRY, 0);
  if (runtime.registry == nullptr) {
    failRuntime(runtime, systemError("cannot access PipeWire registry", -errno));
    return false;
  }
  runtime.registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
  runtime.registryEvents.global = registryGlobal;
  runtime.registryEvents.global_remove = registryGlobalRemoved;
  const auto registryListenerResult =
      pw_registry_add_listener(runtime.registry, &runtime.registryListener,
                               &runtime.registryEvents, &runtime);
  if (registryListenerResult < 0) {
    failRuntime(runtime, systemError("cannot monitor PipeWire registry",
                                     registryListenerResult));
    return false;
  }
  requestTrackingSync(runtime, CoreSyncPurpose::enumeration);
  return runtime.error.empty();
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
  if (options.initialPresetPath.string().find('\0') != std::string::npos ||
      options.controlSocketPath.string().find('\0') != std::string::npos) {
    return "preset and control socket paths must not contain NUL";
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

PipeWireRunResult runPipeWirePipeline(std::unique_ptr<DspPipeline> pipeline,
                                      const PipeWirePipelineOptions &options,
                                      PipeWireRunMode mode) {
  if (pipeline == nullptr) {
    return validationError("DSP pipeline must not be null");
  }
  const auto validation = validateOptions(*pipeline, options);
  if (!validation.empty()) {
    return validationError(validation);
  }

  try {
    auto library = PipeWireLibraryScope{};
    auto runtime = PipeWireRuntime(std::move(pipeline), options, mode);
    if (createMainLoop(runtime) && configureCompletionSources(runtime) &&
        createControlServer(runtime) && createStreams(runtime)) {
      const auto runResult = pw_main_loop_run(runtime.mainLoop);
      if (runResult < 0 && runtime.error.empty()) {
        failRuntime(runtime, systemError("PipeWire main loop failed", runResult));
      } else if (!runtime.completed && runtime.error.empty()) {
        failRuntime(runtime, "PipeWire main loop stopped before completion");
      }
    }
    const auto selectedTarget = currentPlaybackTarget(runtime);
    return {.success = runtime.completed && runtime.error.empty(),
            .error = runtime.error,
            .overrunFrames = runtime.ring.overrunFrames(),
            .underrunFrames = runtime.ring.underrunFrames(),
            .processingErrors =
                runtime.processingErrors.load(std::memory_order_relaxed),
            .selectedTarget = selectedTarget};
  } catch (const std::exception &error) {
    return validationError(std::string("cannot prepare PipeWire pipeline: ") +
                           error.what());
  }
}

} // namespace pipetune
