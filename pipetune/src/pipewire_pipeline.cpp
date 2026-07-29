#include "pipetune/pipewire_pipeline.h"

#include "audio_bridge.h"
#include "default_sink_restore.h"
#include "dsp_pipeline_slot.h"
#include "input_telemetry.h"
#include "output_device_tracker.h"
#include "pipewire_rate_parser.h"
#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/node.h>
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
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace pipetune {

constexpr auto kSampleBytes = std::uint32_t{sizeof(float)};
constexpr auto kReadinessTimeoutSeconds = std::time_t{5};
constexpr auto kRateTransitionTimeoutSeconds = std::time_t{5};

struct PipeWireRuntime;

struct TrackedOutputNode {
  PipeWireRuntime *runtime;
  std::uint32_t id;
  pw_node *node;
  pw_node_events events;
  spa_hook listener;
  std::vector<SampleRateConstraint> pendingConstraints;
  PipeWireRateParameterAvailability parameterAvailability;
};

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

enum class RateTransitionPhase {
  idle,
  applying,
  rollingBack
};

struct RateTransition {
  RateTransitionPhase phase = RateTransitionPhase::idle;
  SampleRatePolicy previousPolicy = defaultSampleRatePolicy();
  ResolvedSampleRates previousRates = {};
  SampleRatePolicy requestedPolicy = defaultSampleRatePolicy();
  ResolvedSampleRates requestedRates = {};
  bool controlRequest = false;
  std::string failure = {};
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
  std::atomic<std::uint32_t> dspSampleRate;
  std::atomic<std::uint32_t> outputSampleRate;
  std::atomic<SampleRateEnforcement> rateEnforcement;
  std::uint64_t processedInputFrames;
  ProcessingMode processingMode;
  std::string activePreset;
  std::string configurationError;
  std::mutex outputStateMutex;
  std::mutex playbackTargetMutex;
  std::mutex rateStateMutex;
  std::mutex pipelineMutationMutex;
  std::mutex rateRequestMutex;
  std::condition_variable rateRequestCondition;
  SampleRatePolicy configuredRatePolicy;
  ResolvedSampleRates resolvedSampleRates;
  bool rateTransitioning;
  std::string rateError;
  bool rateRequestPending;
  bool rateRequestDispatched;
  bool rateRequestCompleted;
  SampleRatePolicy pendingRatePolicy;
  std::string rateRequestError;
  bool automaticRateUpdatePending;
  RateTransition rateTransition;
  std::unique_ptr<ControlServer> controlServer;
  pw_main_loop *mainLoop;
  pw_context *context;
  pw_stream *captureStream;
  pw_stream *playbackStream;
  pw_core *trackingCore;
  pw_registry *registry;
  std::unordered_map<std::uint32_t, std::unique_ptr<TrackedOutputNode>>
      trackedOutputNodes;
  pw_metadata *defaultMetadata;
  spa_source *outputChangeSource;
  spa_source *rateChangeSource;
  spa_source *rateTimeoutSource;
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
  spa_hook captureListener;
  spa_hook playbackListener;
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
  bool captureFormatReady;
  bool playbackFormatReady;
  bool captureListenerInstalled;
  bool playbackListenerInstalled;
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
        inputFormatNegotiated(false),
        dspSampleRate(runtimeOptions.dspSampleRate),
        outputSampleRate(runtimeOptions.outputSampleRate),
        rateEnforcement(runtimeOptions.ratePolicy.enforcement),
        processedInputFrames(0),
        processingMode(runtimeOptions.initialPresetPath.empty()
                           ? ProcessingMode::bypass
                           : ProcessingMode::preset),
        activePreset(runtimeOptions.initialPresetPath.string()),
        configurationError(runtimeOptions.initialConfigurationError),
        outputStateMutex(), playbackTargetMutex(), rateStateMutex(),
        pipelineMutationMutex(), rateRequestMutex(), rateRequestCondition(),
        configuredRatePolicy(runtimeOptions.ratePolicy),
        resolvedSampleRates{.dspSampleRate = runtimeOptions.dspSampleRate,
                            .outputSampleRate =
                                runtimeOptions.outputSampleRate,
                            .fallback = false},
        rateTransitioning(false), rateError(), rateRequestPending(false),
        rateRequestDispatched(false), rateRequestCompleted(false),
        pendingRatePolicy(defaultSampleRatePolicy()), rateRequestError(),
        automaticRateUpdatePending(false), rateTransition(), controlServer(),
        mainLoop(nullptr), context(nullptr),
        captureStream(nullptr), playbackStream(nullptr), trackingCore(nullptr),
        registry(nullptr), trackedOutputNodes(), defaultMetadata(nullptr),
        outputChangeSource(nullptr), rateChangeSource(nullptr),
        rateTimeoutSource(nullptr),
        timeoutSource(nullptr), interruptSource(nullptr),
        terminateSource(nullptr), captureEvents{},
        playbackEvents{}, coreEvents{}, registryEvents{}, metadataEvents{},
        coreListener{}, registryListener{}, metadataListener{},
        captureListener{}, playbackListener{},
        captureContext{this, true}, playbackContext{this, false},
        defaultMetadataId(PW_ID_ANY), trackingSyncSequence(0),
        trackingSyncPurpose(CoreSyncPurpose::enumeration),
        defaultSinkTransition(DefaultSinkTransition::inactive),
        trackingReady(false), shutdownRequested(false),
        observedDefaultSink(), defaultMetadataValue(), playbackTarget(),
        captureReady(false), playbackReady(false), captureFormatReady(false),
        playbackFormatReady(false), captureListenerInstalled(false),
        playbackListenerInstalled(false), readyNotified(false),
        completed(false), error() {}

  ~PipeWireRuntime() {
    controlServer.reset();
    if (playbackStream != nullptr) {
      if (playbackListenerInstalled) {
        spa_hook_remove(&playbackListener);
      }
      pw_stream_destroy(playbackStream);
    }
    if (captureStream != nullptr) {
      if (captureListenerInstalled) {
        spa_hook_remove(&captureListener);
      }
      pw_stream_destroy(captureStream);
    }
    if (defaultMetadata != nullptr) {
      spa_hook_remove(&metadataListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(defaultMetadata));
    }
    for (auto &[id, tracked] : trackedOutputNodes) {
      static_cast<void>(id);
      spa_hook_remove(&tracked->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(tracked->node));
    }
    trackedOutputNodes.clear();
    if (registry != nullptr) {
      spa_hook_remove(&registryListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    }
    if (trackingCore != nullptr) {
      spa_hook_remove(&coreListener);
      pw_core_disconnect(trackingCore);
    }
    if (context != nullptr) {
      pw_context_destroy(context);
    }
    if (mainLoop != nullptr) {
      auto *loop = pw_main_loop_get_loop(mainLoop);
      if (timeoutSource != nullptr) {
        pw_loop_destroy_source(loop, timeoutSource);
      }
      if (outputChangeSource != nullptr) {
        pw_loop_destroy_source(loop, outputChangeSource);
      }
      if (rateChangeSource != nullptr) {
        pw_loop_destroy_source(loop, rateChangeSource);
      }
      if (rateTimeoutSource != nullptr) {
        pw_loop_destroy_source(loop, rateTimeoutSource);
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
static void maybeFinishRateTransition(PipeWireRuntime &runtime);
static void requestAutomaticRateUpdate(PipeWireRuntime &runtime);
static void reportStreamFailure(PipeWireRuntime &runtime,
                                std::string message);
static void stopTrackingOutputNode(PipeWireRuntime &runtime,
                                   std::uint32_t id);

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
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    runtime.deviceTracker.commitSelection();
  }
  applyTrackedTarget(runtime);
  requestAutomaticRateUpdate(runtime);
  if (runtime.options.manageDefaultSink &&
      runtime.defaultMetadata == nullptr) {
    failRuntime(runtime, "PipeWire default metadata is unavailable");
    return;
  }
  maybeActivateDefaultSink(runtime);
}

static bool resetTrackedOutputRateState(TrackedOutputNode &tracked) {
  auto &runtime = *tracked.runtime;
  auto changed = false;
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    changed = runtime.deviceTracker.updateSampleRateCapabilities(
                  tracked.id, {.known = false, .constraints = {}}) ||
              changed;
    changed =
        runtime.deviceTracker.updateActiveSampleRate(tracked.id, 0) || changed;
  }
  if (changed && runtime.trackingReady) {
    requestControlStatusUpdate(runtime);
    requestAutomaticRateUpdate(runtime);
  }
  return changed;
}

static void trackingCoreError(void *data, std::uint32_t id, int, int result,
                              const char *message) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  for (auto iterator = runtime.trackedOutputNodes.begin();
       iterator != runtime.trackedOutputNodes.end(); ++iterator) {
    auto *proxy =
        reinterpret_cast<pw_proxy *>(iterator->second->node);
    if (pw_proxy_get_id(proxy) != id) {
      continue;
    }
    const auto globalId = iterator->first;
    static_cast<void>(resetTrackedOutputRateState(*iterator->second));
    stopTrackingOutputNode(runtime, globalId);
    return;
  }
  const auto detail = message == nullptr ? systemError("PipeWire core error", result)
                                         : std::string(message);
  if (runtime.rateTransition.phase != RateTransitionPhase::idle) {
    reportStreamFailure(runtime,
                        "PipeWire rate negotiation failed: " + detail);
    return;
  }
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
  auto changed = false;
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    changed =
        runtime.deviceTracker.setDefaultTarget(runtime.observedDefaultSink);
  }
  if (changed && runtime.trackingReady) {
    applyTrackedTarget(runtime);
  }
  if (runtime.trackingReady) {
    requestControlStatusUpdate(runtime);
  }
  if (runtime.options.manageDefaultSink && !runtime.shutdownRequested &&
      runtime.defaultSinkTransition == DefaultSinkTransition::active &&
      runtime.observedDefaultSink != runtime.options.sinkName) {
    runtime.defaultSinkTransition = DefaultSinkTransition::inactive;
    maybeActivateDefaultSink(runtime);
  }
  return 0;
}

static void publishOutputRateCapabilities(
    TrackedOutputNode &tracked, SampleRateCapabilities capabilities) {
  auto &runtime = *tracked.runtime;
  auto changed = false;
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    changed = runtime.deviceTracker.updateSampleRateCapabilities(
        tracked.id, std::move(capabilities));
  }
  if (changed && runtime.trackingReady) {
    requestControlStatusUpdate(runtime);
    requestAutomaticRateUpdate(runtime);
  }
}

static void outputNodeParameter(void *data, int, std::uint32_t id,
                                std::uint32_t index, std::uint32_t next,
                                const spa_pod *parameter) {
  auto &tracked = *static_cast<TrackedOutputNode *>(data);
  auto &runtime = *tracked.runtime;
  if (id == SPA_PARAM_EnumFormat) {
    static_cast<void>(next);
    publishOutputRateCapabilities(
        tracked, accumulatePipeWireSampleRateCapabilities(
                     parameter, index, tracked.pendingConstraints));
    return;
  }
  if (id != SPA_PARAM_Format) {
    return;
  }

  auto activeRate = std::uint32_t{0};
  if (parameter != nullptr) {
    auto format = spa_audio_info_raw{};
    if (spa_format_audio_raw_parse(parameter, &format) >= 0) {
      activeRate = format.rate;
    }
  }
  auto changed = false;
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    changed = runtime.deviceTracker.updateActiveSampleRate(
        tracked.id, activeRate);
  }
  if (changed && runtime.trackingReady) {
    requestControlStatusUpdate(runtime);
  }
}

static void outputNodeInfo(void *data, const pw_node_info *info) {
  auto &tracked = *static_cast<TrackedOutputNode *>(data);
  if (info == nullptr ||
      (info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) == 0) {
    return;
  }
  const auto availability =
      pipeWireRateParameterAvailability(info->params, info->n_params);
  if (availability == tracked.parameterAvailability) {
    return;
  }

  auto parameterIds = std::array<std::uint32_t, 2>{};
  auto parameterCount = std::size_t{0};
  if (availability.enumFormatReadable) {
    parameterIds[parameterCount++] = SPA_PARAM_EnumFormat;
  }
  if (availability.formatReadable) {
    parameterIds[parameterCount++] = SPA_PARAM_Format;
  }
  if (pw_node_subscribe_params(tracked.node, parameterIds.data(),
                               parameterCount) < 0) {
    static_cast<void>(resetTrackedOutputRateState(tracked));
    return;
  }

  if (availability.enumFormatReadable &&
      !tracked.parameterAvailability.enumFormatReadable) {
    if (pw_node_enum_params(
            tracked.node, 1, SPA_PARAM_EnumFormat, 0,
            std::numeric_limits<std::uint32_t>::max(), nullptr) < 0) {
      auto &runtime = *tracked.runtime;
      auto changed = false;
      {
        auto lock = std::scoped_lock(runtime.outputStateMutex);
        changed = runtime.deviceTracker.updateSampleRateCapabilities(
            tracked.id, {.known = false, .constraints = {}});
      }
      if (changed && runtime.trackingReady) {
        requestControlStatusUpdate(runtime);
      }
    }
  }
  if (availability.formatReadable &&
      !tracked.parameterAvailability.formatReadable) {
    if (pw_node_enum_params(
            tracked.node, 2, SPA_PARAM_Format, 0,
            std::numeric_limits<std::uint32_t>::max(), nullptr) < 0) {
      auto &runtime = *tracked.runtime;
      auto changed = false;
      {
        auto lock = std::scoped_lock(runtime.outputStateMutex);
        changed =
            runtime.deviceTracker.updateActiveSampleRate(tracked.id, 0);
      }
      if (changed && runtime.trackingReady) {
        requestControlStatusUpdate(runtime);
      }
    }
  } else if (!availability.formatReadable) {
    auto &runtime = *tracked.runtime;
    auto changed = false;
    {
      auto lock = std::scoped_lock(runtime.outputStateMutex);
      changed = runtime.deviceTracker.updateActiveSampleRate(tracked.id, 0);
    }
    if (changed && runtime.trackingReady) {
      requestControlStatusUpdate(runtime);
    }
  }
  tracked.parameterAvailability = availability;
}

static void stopTrackingOutputNode(PipeWireRuntime &runtime,
                                   std::uint32_t id) {
  const auto found = runtime.trackedOutputNodes.find(id);
  if (found == runtime.trackedOutputNodes.end()) {
    return;
  }
  spa_hook_remove(&found->second->listener);
  pw_proxy_destroy(
      reinterpret_cast<pw_proxy *>(found->second->node));
  runtime.trackedOutputNodes.erase(found);
}

static void startTrackingOutputNode(PipeWireRuntime &runtime,
                                    std::uint32_t id,
                                    std::uint32_t version) {
  if (runtime.trackedOutputNodes.contains(id)) {
    return;
  }
  auto tracked = std::make_unique<TrackedOutputNode>();
  tracked->runtime = &runtime;
  tracked->id = id;
  tracked->node = static_cast<pw_node *>(pw_registry_bind(
      runtime.registry, id, PW_TYPE_INTERFACE_Node,
      std::min(version, static_cast<std::uint32_t>(PW_VERSION_NODE)), 0));
  if (tracked->node == nullptr) {
    return;
  }
  tracked->events = {};
  tracked->events.version = PW_VERSION_NODE_EVENTS;
  tracked->events.info = outputNodeInfo;
  tracked->events.param = outputNodeParameter;
  tracked->listener = {};
  tracked->pendingConstraints = {};
  tracked->parameterAvailability = {
      .enumFormatReadable = false, .formatReadable = false};
  const auto listenerResult =
      pw_node_add_listener(tracked->node, &tracked->listener,
                           &tracked->events, tracked.get());
  if (listenerResult < 0) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(tracked->node));
    return;
  }

  const auto insertion =
      runtime.trackedOutputNodes.emplace(id, std::move(tracked));
  if (!insertion.second) {
    return;
  }
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
    const auto isVirtual = parseBooleanProperty(virtualNode);
    auto changed = false;
    {
      auto lock = std::scoped_lock(runtime.outputStateMutex);
      changed = runtime.deviceTracker.updateDevice(
          {.id = id,
           .name = name,
           .description = description,
           .priority = parsePriority(priority),
           .virtualNode = isVirtual});
    }
    if (!isVirtual && std::string_view(name) != runtime.options.sinkName) {
      startTrackingOutputNode(runtime, id, version);
    }
    if (changed && runtime.trackingReady) {
      applyTrackedTarget(runtime);
    }
    if (runtime.trackingReady) {
      requestControlStatusUpdate(runtime);
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
  stopTrackingOutputNode(runtime, id);
  auto changed = false;
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    changed = runtime.deviceTracker.removeDevice(id);
  }
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
  if (runtime.trackingReady) {
    requestControlStatusUpdate(runtime);
  }
}

static bool isReadyState(pw_stream_state state) noexcept {
  return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
}

static void finishReadinessCheck(PipeWireRuntime &runtime) {
  if (!runtime.captureReady || !runtime.playbackReady ||
      !runtime.captureFormatReady || !runtime.playbackFormatReady ||
      !runtime.inputFormatNegotiated.load(std::memory_order_acquire) ||
      runtime.rateTransition.phase != RateTransitionPhase::idle ||
      runtime.automaticRateUpdatePending) {
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
    const auto detail = error == nullptr ? std::string("unknown PipeWire stream error")
                                         : std::string(error);
    if (context.capture) {
      runtime.captureReady = false;
      runtime.captureFormatReady = false;
      runtime.inputFormatNegotiated.store(false, std::memory_order_release);
    } else {
      runtime.playbackReady = false;
      runtime.playbackFormatReady = false;
    }
    if (!context.capture &&
        runtime.rateTransition.phase == RateTransitionPhase::idle) {
      return;
    }
    reportStreamFailure(
        runtime,
        (context.capture ? "virtual sink: " : "playback stream: ") + detail);
    return;
  }
  if (context.capture) {
    runtime.captureReady = isReadyState(state);
    runtime.inputFormatNegotiated.store(
        runtime.captureReady && runtime.captureFormatReady,
        std::memory_order_release);
  } else {
    runtime.playbackReady = isReadyState(state);
  }
  maybeFinishRateTransition(runtime);
  finishReadinessCheck(runtime);
}

static spa_audio_info_raw makeRawFormat(const PipeWirePipelineOptions &options,
                                        std::uint32_t sampleRate) {
  auto info = spa_audio_info_raw{};
  info.format = SPA_AUDIO_FORMAT_F32P;
  info.rate = sampleRate;
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
  const auto expectedSampleRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  if (parseResult < 0 || negotiated.format != SPA_AUDIO_FORMAT_F32P ||
      negotiated.rate != expectedSampleRate ||
      negotiated.channels != runtime.options.channelCount) {
    reportStreamFailure(runtime,
                        "PipeWire negotiated an unsupported audio format");
    return;
  }

  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const spa_pod *parameters[] = {buildBufferParameter(builder, runtime.options)};
  auto *stream = context.capture ? runtime.captureStream : runtime.playbackStream;
  if (stream == nullptr) {
    return;
  }
  const auto updateResult = pw_stream_update_params(stream, parameters, 1);
  if (updateResult < 0) {
    reportStreamFailure(
        runtime,
        systemError("cannot configure PipeWire buffers", updateResult));
    return;
  }
  if (context.capture) {
    runtime.captureFormatReady = true;
    runtime.inputFormatNegotiated.store(
        runtime.captureReady, std::memory_order_release);
  } else {
    runtime.playbackFormatReady = true;
  }
  maybeFinishRateTransition(runtime);
  finishReadinessCheck(runtime);
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

    const auto sampleRate =
        runtime.dspSampleRate.load(std::memory_order_relaxed);
    const auto timeSeconds =
        static_cast<double>(runtime.processedInputFrames) / sampleRate;
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
                                           std::string_view nodeName,
                                           std::uint32_t mediaSampleRate,
                                           std::uint32_t nodeSampleRate) {
  auto *properties = pw_properties_new(nullptr, nullptr);
  if (properties == nullptr) {
    return nullptr;
  }
  const auto mediaRate = std::to_string(mediaSampleRate);
  const auto channels = std::to_string(runtime.options.channelCount);
  const auto nodeRate = "1/" + std::to_string(nodeSampleRate);
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
  pw_properties_set(properties, SPA_KEY_AUDIO_RATE, mediaRate.c_str());
  pw_properties_set(properties, SPA_KEY_AUDIO_CHANNELS, channels.c_str());
  return properties;
}

static pw_properties *makeCaptureProperties(const PipeWireRuntime &runtime) {
  const auto sampleRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  auto *properties = makeCommonProperties(
      runtime, runtime.options.sinkName, sampleRate, sampleRate);
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
  auto *properties = makeCommonProperties(
      runtime, runtime.options.sinkName + ".output",
      runtime.dspSampleRate.load(std::memory_order_acquire),
      runtime.outputSampleRate.load(std::memory_order_acquire));
  if (properties == nullptr) {
    return nullptr;
  }
  pw_properties_set(properties, PW_KEY_MEDIA_CLASS, "Stream/Output/Audio");
  pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
  pw_properties_set(properties, PW_KEY_NODE_PASSIVE, "true");
  pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                    std::string(target).c_str());
  if (runtime.rateEnforcement.load(std::memory_order_acquire) ==
      SampleRateEnforcement::force) {
    pw_properties_set(properties, PW_KEY_NODE_FORCE_RATE, "0");
  }
  return properties;
}

static std::string connectStream(PipeWireRuntime &runtime, pw_stream *stream,
                                 pw_direction direction, bool autoconnect,
                                 bool dontReconnect) {
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  auto info = makeRawFormat(
      runtime.options,
      runtime.dspSampleRate.load(std::memory_order_acquire));
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
    return systemError("cannot connect PipeWire stream", result);
  }
  return {};
}

static std::string createPlaybackStream(PipeWireRuntime &runtime,
                                        std::string_view target) {
  auto *properties = makePlaybackProperties(runtime, target);
  if (properties == nullptr) {
    return "cannot allocate PipeWire playback properties";
  }
  runtime.playbackStream =
      pw_stream_new(runtime.trackingCore, "PipeTune playback", properties);
  if (runtime.playbackStream == nullptr) {
    return systemError("cannot create PipeWire playback stream", -errno);
  }
  runtime.playbackListener = {};
  pw_stream_add_listener(runtime.playbackStream,
                         &runtime.playbackListener,
                         &runtime.playbackEvents,
                         &runtime.playbackContext);
  runtime.playbackListenerInstalled = true;
  const auto connectionError =
      connectStream(runtime, runtime.playbackStream, PW_DIRECTION_OUTPUT, true,
                    true);
  if (!connectionError.empty()) {
    spa_hook_remove(&runtime.playbackListener);
    runtime.playbackListenerInstalled = false;
    pw_stream_destroy(runtime.playbackStream);
    runtime.playbackStream = nullptr;
    return connectionError;
  }
  return {};
}

static void destroyPlaybackStream(PipeWireRuntime &runtime) {
  if (runtime.playbackStream == nullptr) {
    return;
  }
  if (runtime.playbackListenerInstalled) {
    spa_hook_remove(&runtime.playbackListener);
    runtime.playbackListenerInstalled = false;
  }
  pw_stream_destroy(runtime.playbackStream);
  runtime.playbackStream = nullptr;
}

static void signalRateChange(PipeWireRuntime &runtime) {
  if (runtime.mainLoop == nullptr || runtime.rateChangeSource == nullptr) {
    return;
  }
  const auto result = pw_loop_signal_event(
      pw_main_loop_get_loop(runtime.mainLoop), runtime.rateChangeSource);
  if (result < 0) {
    failRuntime(runtime,
                systemError("cannot schedule PipeWire rate change", result));
  }
}

static void completeControlRateRequest(PipeWireRuntime &runtime,
                                       std::string error) {
  {
    auto lock = std::scoped_lock(runtime.rateRequestMutex);
    if (!runtime.rateRequestPending ||
        !runtime.rateRequestDispatched) {
      return;
    }
    runtime.rateRequestError = std::move(error);
    runtime.rateRequestCompleted = true;
  }
  runtime.rateRequestCondition.notify_all();
}

static SampleRateCapabilities
selectedRateCapabilities(PipeWireRuntime &runtime) {
  auto lock = std::scoped_lock(runtime.outputStateMutex);
  return runtime.deviceTracker.selectedSampleRateCapabilities();
}

static void setPublicRateState(PipeWireRuntime &runtime,
                               const SampleRatePolicy *policy,
                               const ResolvedSampleRates &rates,
                               bool transitioning,
                               std::string error) {
  auto lock = std::scoped_lock(runtime.rateStateMutex);
  if (policy != nullptr) {
    runtime.configuredRatePolicy = *policy;
  }
  runtime.resolvedSampleRates = rates;
  runtime.rateTransitioning = transitioning;
  runtime.rateError = std::move(error);
}

static std::string armRateTransitionTimer(PipeWireRuntime &runtime,
                                          bool armed) {
  if (runtime.rateTimeoutSource == nullptr) {
    return "rate-transition timer is unavailable";
  }
  auto *loop = pw_main_loop_get_loop(runtime.mainLoop);
  if (!armed) {
    const auto result = pw_loop_update_timer(
        loop, runtime.rateTimeoutSource, nullptr, nullptr, false);
    if (result < 0) {
      return systemError("cannot disarm rate-transition timer", result);
    }
    return {};
  }
  auto delay =
      timespec{.tv_sec = kRateTransitionTimeoutSeconds, .tv_nsec = 0};
  auto interval = timespec{.tv_sec = 0, .tv_nsec = 0};
  const auto result = pw_loop_update_timer(
      loop, runtime.rateTimeoutSource, &delay, &interval, false);
  if (result < 0) {
    return systemError("cannot arm rate-transition timer", result);
  }
  return {};
}

static std::string disconnectAudioStreams(PipeWireRuntime &runtime) {
  if (runtime.captureStream != nullptr &&
      pw_stream_get_state(runtime.captureStream, nullptr) !=
          PW_STREAM_STATE_UNCONNECTED) {
    const auto result = pw_stream_disconnect(runtime.captureStream);
    if (result < 0) {
      return systemError("cannot disconnect PipeWire virtual sink", result);
    }
  }
  if (runtime.playbackStream != nullptr) {
    destroyPlaybackStream(runtime);
  }
  runtime.captureReady = false;
  runtime.playbackReady = false;
  runtime.captureFormatReady = false;
  runtime.playbackFormatReady = false;
  runtime.inputFormatNegotiated.store(false, std::memory_order_release);
  static_cast<void>(runtime.ring.discardQueuedFrames());
  runtime.processedInputFrames = 0;
  return {};
}

static std::string reconnectAudioStreams(PipeWireRuntime &runtime) {
  auto *captureProperties = makeCaptureProperties(runtime);
  if (captureProperties == nullptr) {
    return "cannot allocate PipeWire virtual sink properties";
  }
  const auto propertyResult = pw_stream_update_properties(
      runtime.captureStream, &captureProperties->dict);
  pw_properties_free(captureProperties);
  if (propertyResult < 0) {
    return systemError("cannot update PipeWire virtual sink properties",
                       propertyResult);
  }
  const auto captureError =
      connectStream(runtime, runtime.captureStream, PW_DIRECTION_INPUT, false,
                    false);
  if (!captureError.empty()) {
    return captureError;
  }

  const auto target = currentPlaybackTarget(runtime);
  if (target.empty()) {
    return {};
  }
  return createPlaybackStream(runtime, target);
}

static bool rateTransitionAudioIsReady(PipeWireRuntime &runtime) {
  if (!runtime.captureReady || !runtime.captureFormatReady) {
    return false;
  }
  return currentPlaybackTarget(runtime).empty() ||
         (runtime.playbackReady && runtime.playbackFormatReady);
}

static void failRateAttempt(PipeWireRuntime &runtime, bool controlRequest,
                            std::string error) {
  auto rates = ResolvedSampleRates{};
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    rates = runtime.resolvedSampleRates;
  }
  setPublicRateState(runtime, nullptr, rates, false, error);
  if (controlRequest) {
    completeControlRateRequest(runtime, error);
  }
  requestControlStatusUpdate(runtime);
  finishReadinessCheck(runtime);
}

static void fatalRateRollback(PipeWireRuntime &runtime,
                              std::string error) {
  const auto detail = "cannot roll back sample-rate transition: " + error;
  if (runtime.rateTransition.controlRequest) {
    completeControlRateRequest(runtime, detail);
  }
  failRuntime(runtime, detail);
}

static void beginRateRollback(PipeWireRuntime &runtime) {
  if (runtime.rateTransition.phase == RateTransitionPhase::rollingBack) {
    fatalRateRollback(runtime, runtime.rateTransition.failure);
    return;
  }
  const auto failure = runtime.rateTransition.failure.empty()
                           ? std::string("sample-rate transition failed")
                           : runtime.rateTransition.failure;
  const auto timerError = armRateTransitionTimer(runtime, false);
  if (!timerError.empty()) {
    fatalRateRollback(runtime, timerError);
    return;
  }
  const auto disconnectError = disconnectAudioStreams(runtime);
  if (!disconnectError.empty()) {
    fatalRateRollback(runtime, disconnectError);
    return;
  }
  {
    auto lock = std::scoped_lock(runtime.pipelineMutationMutex);
    try {
      runtime.pipeline.rollbackStaged();
    } catch (const std::exception &error) {
      fatalRateRollback(runtime, error.what());
      return;
    }
  }

  runtime.rateTransition.phase = RateTransitionPhase::rollingBack;
  runtime.rateTransition.failure = failure;
  runtime.dspSampleRate.store(
      runtime.rateTransition.previousRates.dspSampleRate,
      std::memory_order_release);
  runtime.outputSampleRate.store(
      runtime.rateTransition.previousRates.outputSampleRate,
      std::memory_order_release);
  runtime.rateEnforcement.store(
      runtime.rateTransition.previousPolicy.enforcement,
      std::memory_order_release);
  setPublicRateState(runtime, nullptr,
                     runtime.rateTransition.previousRates, true, failure);
  const auto reconnectError = reconnectAudioStreams(runtime);
  if (!reconnectError.empty()) {
    fatalRateRollback(runtime, reconnectError);
    return;
  }
  const auto armError = armRateTransitionTimer(runtime, true);
  if (!armError.empty()) {
    fatalRateRollback(runtime, armError);
    return;
  }
  requestControlStatusUpdate(runtime);
}

static void finishRateTransition(PipeWireRuntime &runtime) {
  const auto transition = runtime.rateTransition;
  const auto timerError = armRateTransitionTimer(runtime, false);
  if (!timerError.empty()) {
    if (transition.phase == RateTransitionPhase::applying) {
      runtime.rateTransition.failure = timerError;
      beginRateRollback(runtime);
    } else {
      fatalRateRollback(runtime, timerError);
    }
    return;
  }

  if (transition.phase == RateTransitionPhase::applying) {
    auto commitError = std::string{};
    {
      auto lock = std::scoped_lock(runtime.pipelineMutationMutex);
      try {
        runtime.pipeline.commitStaged();
      } catch (const std::exception &error) {
        commitError = error.what();
      }
    }
    if (!commitError.empty()) {
      runtime.rateTransition.failure = std::move(commitError);
      beginRateRollback(runtime);
      return;
    }
    runtime.rateTransition = {};
    setPublicRateState(runtime, &transition.requestedPolicy,
                       transition.requestedRates, false, {});
    if (transition.controlRequest) {
      completeControlRateRequest(runtime, {});
    }
  } else {
    runtime.rateTransition = {};
    setPublicRateState(runtime, &transition.previousPolicy,
                       transition.previousRates, false,
                       transition.failure);
    if (transition.controlRequest) {
      completeControlRateRequest(runtime, transition.failure);
    }
  }
  requestControlStatusUpdate(runtime);
  signalRateChange(runtime);
  finishReadinessCheck(runtime);
}

static void maybeFinishRateTransition(PipeWireRuntime &runtime) {
  if (runtime.rateTransition.phase == RateTransitionPhase::idle ||
      !rateTransitionAudioIsReady(runtime)) {
    return;
  }
  finishRateTransition(runtime);
}

static void startRateTransition(PipeWireRuntime &runtime,
                                const SampleRatePolicy &requestedPolicy,
                                bool controlRequest) {
  auto previousPolicy = SampleRatePolicy{};
  auto previousRates = ResolvedSampleRates{};
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    previousPolicy = runtime.configuredRatePolicy;
    previousRates = runtime.resolvedSampleRates;
  }
  const auto resolved =
      resolveSampleRates(requestedPolicy,
                         selectedRateCapabilities(runtime),
                         previousRates.dspSampleRate,
                         previousRates.outputSampleRate);
  if (!resolved.has_value()) {
    failRateAttempt(runtime, controlRequest,
                    "cannot resolve the requested sample-rate policy");
    return;
  }

  const auto needsReconnect =
      resolved->dspSampleRate != previousRates.dspSampleRate ||
      resolved->outputSampleRate != previousRates.outputSampleRate ||
      requestedPolicy.enforcement != previousPolicy.enforcement;
  if (!needsReconnect) {
    runtime.dspSampleRate.store(resolved->dspSampleRate,
                                std::memory_order_release);
    runtime.outputSampleRate.store(resolved->outputSampleRate,
                                   std::memory_order_release);
    runtime.rateEnforcement.store(requestedPolicy.enforcement,
                                  std::memory_order_release);
    setPublicRateState(runtime, &requestedPolicy, *resolved, false, {});
    if (controlRequest) {
      completeControlRateRequest(runtime, {});
    }
    requestControlStatusUpdate(runtime);
    signalRateChange(runtime);
    finishReadinessCheck(runtime);
    return;
  }

  auto pipelineLock =
      std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
  auto rebuilt = runtime.pipeline.rebuildActive(
      {.sampleRate = static_cast<float>(resolved->dspSampleRate),
       .maxChannels = runtime.options.channelCount,
       .maxFrames = runtime.options.maxFrames});
  if (rebuilt.pipeline == nullptr) {
    pipelineLock.unlock();
    failRateAttempt(runtime, controlRequest, rebuilt.error);
    return;
  }
  const auto disconnectError = disconnectAudioStreams(runtime);
  if (!disconnectError.empty()) {
    pipelineLock.unlock();
    failRateAttempt(runtime, controlRequest, disconnectError);
    return;
  }

  runtime.pipeline.stageReplacement(std::move(rebuilt.pipeline));
  runtime.rateTransition = {
      .phase = RateTransitionPhase::applying,
      .previousPolicy = previousPolicy,
      .previousRates = previousRates,
      .requestedPolicy = requestedPolicy,
      .requestedRates = *resolved,
      .controlRequest = controlRequest,
      .failure = {}};
  runtime.dspSampleRate.store(resolved->dspSampleRate,
                              std::memory_order_release);
  runtime.outputSampleRate.store(resolved->outputSampleRate,
                                 std::memory_order_release);
  runtime.rateEnforcement.store(requestedPolicy.enforcement,
                                std::memory_order_release);
  setPublicRateState(runtime, nullptr, *resolved, true, {});
  pipelineLock.unlock();

  const auto reconnectError = reconnectAudioStreams(runtime);
  if (!reconnectError.empty()) {
    runtime.rateTransition.failure = reconnectError;
    beginRateRollback(runtime);
    return;
  }
  const auto armError = armRateTransitionTimer(runtime, true);
  if (!armError.empty()) {
    runtime.rateTransition.failure = armError;
    beginRateRollback(runtime);
    return;
  }
  requestControlStatusUpdate(runtime);
}

static void reportStreamFailure(PipeWireRuntime &runtime,
                                std::string message) {
  if (runtime.rateTransition.phase == RateTransitionPhase::idle) {
    failRuntime(runtime, std::move(message));
    return;
  }
  if (runtime.rateTransition.phase ==
      RateTransitionPhase::rollingBack) {
    fatalRateRollback(runtime, std::move(message));
    return;
  }
  if (runtime.rateTransition.failure.empty()) {
    runtime.rateTransition.failure = std::move(message);
  }
  signalRateChange(runtime);
}

static void requestAutomaticRateUpdate(PipeWireRuntime &runtime) {
  if (!runtime.trackingReady || runtime.shutdownRequested) {
    return;
  }
  runtime.automaticRateUpdatePending = true;
  signalRateChange(runtime);
}

static void rateTransitionTimedOut(void *data, std::uint64_t) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  if (runtime.rateTransition.phase == RateTransitionPhase::idle) {
    return;
  }
  if (runtime.rateTransition.phase == RateTransitionPhase::rollingBack) {
    fatalRateRollback(
        runtime,
        "timed out while restoring the previous PipeWire format");
    return;
  }
  runtime.rateTransition.failure =
      "timed out while negotiating the requested PipeWire format";
  beginRateRollback(runtime);
}

static void rateChangeRequested(void *data, std::uint64_t) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  if (runtime.rateTransition.phase != RateTransitionPhase::idle) {
    if (!runtime.rateTransition.failure.empty() &&
        runtime.rateTransition.phase == RateTransitionPhase::applying) {
      beginRateRollback(runtime);
    }
    return;
  }

  auto controlRequest = false;
  auto requestedPolicy = SampleRatePolicy{};
  {
    auto lock = std::scoped_lock(runtime.rateRequestMutex);
    if (runtime.rateRequestPending &&
        !runtime.rateRequestDispatched) {
      runtime.rateRequestDispatched = true;
      controlRequest = true;
      requestedPolicy = runtime.pendingRatePolicy;
    }
  }
  if (!controlRequest) {
    if (!runtime.automaticRateUpdatePending) {
      finishReadinessCheck(runtime);
      return;
    }
    runtime.automaticRateUpdatePending = false;
    {
      auto lock = std::scoped_lock(runtime.rateStateMutex);
      requestedPolicy = runtime.configuredRatePolicy;
    }
  }
  startRateTransition(runtime, requestedPolicy, controlRequest);
}

static void applyTrackedTarget(PipeWireRuntime &runtime) {
  if (!runtime.trackingReady) {
    return;
  }
  auto target = std::string{};
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    target = runtime.deviceTracker.selectedTarget();
  }
  const auto targetUnchanged = target == currentPlaybackTarget(runtime);
  if (targetUnchanged &&
      ((target.empty() && runtime.playbackStream == nullptr) ||
       (!target.empty() && runtime.playbackStream != nullptr))) {
    if (target.empty()) {
      maybeReleaseDefaultSink(runtime);
    }
    requestAutomaticRateUpdate(runtime);
    return;
  }
  if (runtime.playbackStream != nullptr) {
    destroyPlaybackStream(runtime);
  }
  runtime.playbackReady = false;
  runtime.playbackFormatReady = false;
  static_cast<void>(runtime.ring.discardQueuedFrames());
  {
    auto lock = std::scoped_lock(runtime.playbackTargetMutex);
    runtime.playbackTarget = target;
  }
  requestControlStatusUpdate(runtime);
  if (!target.empty()) {
    const auto playbackError = createPlaybackStream(runtime, target);
    if (!playbackError.empty()) {
      reportStreamFailure(runtime, playbackError);
      return;
    }
  }
  requestAutomaticRateUpdate(runtime);
  maybeFinishRateTransition(runtime);
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

static ControlOutputSelectionReason
controlOutputSelectionReason(OutputSelectionReason reason) noexcept {
  switch (reason) {
  case OutputSelectionReason::unavailable:
    return ControlOutputSelectionReason::unavailable;
  case OutputSelectionReason::systemDefault:
    return ControlOutputSelectionReason::systemDefault;
  case OutputSelectionReason::preferred:
    return ControlOutputSelectionReason::preferred;
  case OutputSelectionReason::fallback:
    return ControlOutputSelectionReason::fallback;
  }
  return ControlOutputSelectionReason::unavailable;
}

static ControlRuntimeStatus controlStatus(PipeWireRuntime &runtime) {
  const auto input = snapshotInputTelemetry(
      runtime.inputTelemetry, currentMonotonicNanoseconds(),
      currentUnixMilliseconds());
  const auto inputFormatNegotiated =
      runtime.inputFormatNegotiated.load(std::memory_order_acquire);
  const auto dspPerformance = runtime.pipeline.performanceCounters();
  auto preferredTarget = std::string{};
  auto selectedTarget = std::string{};
  auto systemDefaultTarget = std::string{};
  auto selectionReason = OutputSelectionReason::unavailable;
  auto activeOutputSampleRate = std::uint32_t{0};
  auto devices = std::vector<OutputDevice>{};
  auto configuredRatePolicy = SampleRatePolicy{};
  auto resolvedSampleRates = ResolvedSampleRates{};
  auto rateTransitioning = false;
  auto rateError = std::string{};
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    preferredTarget = runtime.deviceTracker.preferredTarget();
    selectedTarget = runtime.deviceTracker.selectedTarget();
    systemDefaultTarget = runtime.deviceTracker.systemDefaultTarget();
    selectionReason = runtime.deviceTracker.selectionReason();
    activeOutputSampleRate =
        runtime.deviceTracker.selectedActiveSampleRate();
    devices = runtime.deviceTracker.availableDevices();
  }
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    configuredRatePolicy = runtime.configuredRatePolicy;
    resolvedSampleRates = runtime.resolvedSampleRates;
    rateTransitioning = runtime.rateTransitioning;
    rateError = runtime.rateError;
  }
  auto availableOutputs = std::vector<ControlOutputDevice>{};
  availableOutputs.reserve(devices.size());
  for (auto &device : devices) {
    const auto systemDefault = device.name == systemDefaultTarget;
    const auto preferred = device.name == preferredTarget;
    const auto selected = device.name == selectedTarget;
    availableOutputs.push_back(
        {.name = std::move(device.name),
         .description = std::move(device.description),
         .systemDefault = systemDefault,
         .preferred = preferred,
         .selected = selected,
         .sampleRateCapabilities =
             std::move(device.sampleRateCapabilities)});
  }
  return {.processingMode = runtime.processingMode,
          .activePreset = runtime.activePreset,
          .configurationError = runtime.configurationError,
          .activePluginCount = runtime.pipeline.activePluginCount(),
          .preferredTarget = std::move(preferredTarget),
          .selectedTarget = std::move(selectedTarget),
          .outputSelectionReason =
              controlOutputSelectionReason(selectionReason),
          .availableOutputs = std::move(availableOutputs),
          .defaultSinkActive =
              runtime.defaultSinkActive.load(std::memory_order_acquire),
          .overrunFrames = runtime.ring.overrunFrames(),
          .underrunFrames = runtime.ring.underrunFrames(),
          .processingErrors =
              runtime.processingErrors.load(std::memory_order_relaxed),
          .dspProcessedFrames = dspPerformance.processedFrames,
          .dspProcessingNanoseconds =
              dspPerformance.processingNanoseconds,
          .inputSampleFormat =
              inputFormatNegotiated ? std::string("F32P") : std::string{},
          .inputSampleRate =
              inputFormatNegotiated
                  ? runtime.dspSampleRate.load(std::memory_order_acquire)
                  : 0,
          .inputChannelCount =
              inputFormatNegotiated ? runtime.options.channelCount : 0,
          .inputFramesReceived = input.framesReceived,
          .inputLastReceivedUnixMilliseconds =
              input.lastReceivedUnixMilliseconds,
          .configuredRatePolicy = configuredRatePolicy,
          .dspSampleRate = resolvedSampleRates.dspSampleRate,
          .selectedOutputSampleRate =
              resolvedSampleRates.outputSampleRate,
          .activeOutputSampleRate = activeOutputSampleRate,
          .rateTransitioning = rateTransitioning,
          .rateFallback = resolvedSampleRates.fallback,
          .rateError = std::move(rateError)};
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

static std::string changePreferredOutput(PipeWireRuntime &runtime,
                                         std::string_view target,
                                         bool &changed) {
  if (target == runtime.options.sinkName) {
    return "PipeTune cannot use its own virtual sink as an output";
  }
  auto previous = std::string{};
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    previous = runtime.deviceTracker.preferredTarget();
    changed =
        runtime.deviceTracker.setPreferredTarget(std::string(target));
  }
  if (!changed) {
    return {};
  }

  auto *loop = pw_main_loop_get_loop(runtime.mainLoop);
  const auto result =
      pw_loop_signal_event(loop, runtime.outputChangeSource);
  if (result >= 0) {
    return {};
  }
  {
    auto lock = std::scoped_lock(runtime.outputStateMutex);
    static_cast<void>(
        runtime.deviceTracker.setPreferredTarget(std::move(previous)));
  }
  changed = false;
  return systemError("cannot schedule PipeWire output change", result);
}

static std::string requestLiveRateChange(
    PipeWireRuntime &runtime, const SampleRatePolicy &policy) {
  auto lock = std::unique_lock<std::mutex>(runtime.rateRequestMutex);
  if (runtime.rateRequestPending) {
    return "another sample-rate request is already pending";
  }
  runtime.rateRequestPending = true;
  runtime.rateRequestDispatched = false;
  runtime.rateRequestCompleted = false;
  runtime.pendingRatePolicy = policy;
  runtime.rateRequestError.clear();

  const auto signalResult = pw_loop_signal_event(
      pw_main_loop_get_loop(runtime.mainLoop), runtime.rateChangeSource);
  if (signalResult < 0) {
    runtime.rateRequestPending = false;
    return systemError("cannot schedule PipeWire rate change",
                       signalResult);
  }
  runtime.rateRequestCondition.wait(
      lock, [&runtime] { return runtime.rateRequestCompleted; });
  auto error = std::move(runtime.rateRequestError);
  runtime.rateRequestPending = false;
  runtime.rateRequestDispatched = false;
  runtime.rateRequestCompleted = false;
  lock.unlock();
  signalRateChange(runtime);
  return error;
}

static void cancelPendingRateRequest(PipeWireRuntime &runtime,
                                     std::string_view error) {
  {
    auto lock = std::scoped_lock(runtime.rateRequestMutex);
    if (!runtime.rateRequestPending || runtime.rateRequestCompleted) {
      return;
    }
    runtime.rateRequestError = std::string(error);
    runtime.rateRequestCompleted = true;
  }
  runtime.rateRequestCondition.notify_all();
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
  if (request.request.command == ControlCommand::setOutput ||
      request.request.command == ControlCommand::clearOutput) {
    auto changed = false;
    const auto target =
        request.request.command == ControlCommand::setOutput
            ? std::string_view(request.request.outputTarget)
            : std::string_view{};
    const auto error = changePreferredOutput(runtime, target, changed);
    if (!error.empty()) {
      return closeControlResponse(makeControlErrorResponse(error), false);
    }
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), changed);
  }
  if (request.request.command == ControlCommand::setRate) {
    const auto error =
        requestLiveRateChange(runtime, request.request.ratePolicy);
    if (!error.empty()) {
      return closeControlResponse(makeControlErrorResponse(error), false);
    }
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), true);
  }
  if (request.request.command == ControlCommand::bypass) {
    auto pipelineLock =
        std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
    {
      auto rateLock = std::scoped_lock(runtime.rateStateMutex);
      if (runtime.rateTransitioning) {
        return closeControlResponse(
            makeControlErrorResponse(
                "cannot change DSP mode during sample-rate transition"),
            false);
      }
    }
    auto created = createBypassDspPipeline(
        {.sampleRate = static_cast<float>(
             runtime.dspSampleRate.load(std::memory_order_acquire)),
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
    auto pipelineLock =
        std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
    {
      auto rateLock = std::scoped_lock(runtime.rateStateMutex);
      if (runtime.rateTransitioning) {
        return closeControlResponse(
            makeControlErrorResponse(
                "cannot load a preset during sample-rate transition"),
            false);
      }
    }
    auto loaded = loadDspPipeline(
        request.request.presetPath,
        {.sampleRate = static_cast<float>(
             runtime.dspSampleRate.load(std::memory_order_acquire)),
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

static void outputChangeRequested(void *data, std::uint64_t) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  applyTrackedTarget(runtime);
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
  runtime.outputChangeSource =
      pw_loop_add_event(pw_main_loop_get_loop(runtime.mainLoop),
                        outputChangeRequested, &runtime);
  if (runtime.outputChangeSource == nullptr) {
    failRuntime(runtime,
                systemError("cannot create PipeWire output-change event",
                            -errno));
    return false;
  }
  runtime.rateChangeSource =
      pw_loop_add_event(pw_main_loop_get_loop(runtime.mainLoop),
                        rateChangeRequested, &runtime);
  if (runtime.rateChangeSource == nullptr) {
    failRuntime(runtime,
                systemError("cannot create PipeWire rate-change event",
                            -errno));
    return false;
  }
  runtime.rateTimeoutSource =
      pw_loop_add_timer(pw_main_loop_get_loop(runtime.mainLoop),
                        rateTransitionTimedOut, &runtime);
  if (runtime.rateTimeoutSource == nullptr) {
    failRuntime(runtime,
                systemError("cannot create PipeWire rate-transition timer",
                            -errno));
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

  runtime.context = pw_context_new(
      pw_main_loop_get_loop(runtime.mainLoop), nullptr, 0);
  if (runtime.context == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire context", -errno));
    return false;
  }
  runtime.trackingCore = pw_context_connect(runtime.context, nullptr, 0);
  if (runtime.trackingCore == nullptr) {
    failRuntime(runtime, systemError("cannot connect to PipeWire core", -errno));
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

  auto *captureProperties = makeCaptureProperties(runtime);
  if (captureProperties == nullptr) {
    failRuntime(runtime, "cannot allocate PipeWire virtual sink properties");
    return false;
  }
  runtime.captureStream = pw_stream_new(
      runtime.trackingCore, "PipeTune virtual sink", captureProperties);
  if (runtime.captureStream == nullptr) {
    failRuntime(runtime,
                systemError("cannot create PipeWire virtual sink", -errno));
    return false;
  }
  runtime.captureListener = {};
  pw_stream_add_listener(runtime.captureStream,
                         &runtime.captureListener,
                         &runtime.captureEvents,
                         &runtime.captureContext);
  runtime.captureListenerInstalled = true;
  const auto captureConnectionError =
      connectStream(runtime, runtime.captureStream, PW_DIRECTION_INPUT, false,
                    false);
  if (!captureConnectionError.empty()) {
    failRuntime(runtime, captureConnectionError);
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
  if (!isSelectableSampleRate(options.dspSampleRate)) {
    return "DSP sample rate must be 44100, 48000, 96000, 192000, or 384000 Hz";
  }
  if (options.outputSampleRate == 0) {
    return "PipeWire output sample rate must be positive";
  }
  if (!sampleRatePolicyIsValid(options.ratePolicy)) {
    return "sample-rate policy is invalid";
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
  if (pipeline.sampleRate() !=
          static_cast<float>(options.dspSampleRate) ||
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
    cancelPendingRateRequest(
        runtime,
        runtime.error.empty() ? std::string_view("PipeTune daemon stopped")
                              : std::string_view(runtime.error));
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
