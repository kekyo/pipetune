#include "pipetune/pipewire_pipeline.h"

#include "active_preset_file_monitor.h"
#include "audio_bridge.h"
#include "dsp_backend_runtime.h"
#include "dsp_pipeline_slot.h"
#include "filter_graph_properties.h"
#include "input_telemetry.h"
#include "pipewire_buffer_io.h"
#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "sample_rate_converter.h"

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
#include <chrono>
#include <climits>
#include <csignal>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <exception>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune {

constexpr auto kSampleBytes = std::uint32_t{sizeof(float)};
constexpr auto kReadinessTimeoutSeconds = std::time_t{5};
constexpr auto kMinimumGraphSampleRate = std::uint32_t{8000};
constexpr auto kMaximumGraphSampleRate = std::uint32_t{768000};
constexpr auto kPipelineTransitionSilenceMilliseconds = std::uint32_t{20};
constexpr auto kPipelineTransitionFadeMilliseconds = std::uint32_t{5};

static std::uint32_t pipelineTransitionSilenceFrames(
    std::uint32_t sampleRate) noexcept {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(sampleRate) *
           kPipelineTransitionSilenceMilliseconds +
       999) /
      1000);
}

static std::uint32_t pipelineTransitionFadeFrames(
    std::uint32_t sampleRate) noexcept {
  return static_cast<std::uint32_t>(
      (static_cast<std::uint64_t>(sampleRate) *
           kPipelineTransitionFadeMilliseconds +
       999) /
      1000);
}

struct PipeWireRuntime;

struct StreamCallbackContext {
  PipeWireRuntime *runtime;
  bool input;
};

static bool backendDiscoveryWasSupplied(
    const DspBackendLoadResult &result) {
  return result.backend != nullptr || !result.attemptedPath.empty() ||
         !result.cpuRequirement.empty() || !result.error.empty();
}

static DspBackends resolveRuntimeBackends(
    const PipeWirePipelineOptions &options) {
  if (!backendDiscoveryWasSupplied(options.dspBackends.scalar) &&
      !backendDiscoveryWasSupplied(options.dspBackends.simd)) {
    return discoverDspBackends();
  }
  return options.dspBackends;
}

struct PipeWireRuntime {
  DspPipelineSlot pipeline;
  PipeWirePipelineOptions options;
  PipeWireRunMode mode;
  PlanarAudioRing ring;
  AudioTransitionSilencer outputTransitionSilencer;
  PlanarSampleRateConverter inputRateConverter;
  PlanarSampleRateConverter outputRateConverter;
  std::vector<float> inputScratch;
  std::vector<float> dspScratch;
  std::vector<float> convertedScratch;
  std::vector<float> outputScratch;
  std::atomic<std::uint64_t> processingErrors;
  InputTelemetry inputTelemetry;
  std::atomic<bool> rateBridgeReady;
  std::atomic<bool> inputFormatNegotiated;
  std::atomic<std::uint32_t> dspSampleRate;
  std::atomic<std::uint32_t> inputStreamSampleRate;
  std::atomic<std::uint32_t> outputStreamSampleRate;
  std::atomic<std::uint32_t> graphSampleRate;
  std::uint32_t rateBridgeStreamRate;
  std::uint32_t rateBridgeDspRate;
  bool silenceNextRateBridge;
  std::uint64_t processedInputFrames;
  ProcessingMode processingMode;
  std::string activePreset;
  std::string configurationError;
  std::atomic<std::uint64_t> configurationRevision;
  bool presetReloadPending;
  DspBackendRuntimeState dspBackendState;
  std::mutex pipelineMutationMutex;
  std::mutex dspBackendStateMutex;
  std::mutex rateStateMutex;
  std::mutex rateRequestMutex;
  std::condition_variable rateRequestCondition;
  SampleRatePolicy configuredRatePolicy;
  SampleRatePolicy pendingRatePolicy;
  bool rateTransitioning;
  bool rateRequestPending;
  bool rateRequestCompleted;
  std::string rateRequestError;
  std::string rateError;
  std::unique_ptr<ActivePresetFileMonitor> presetFileMonitor;
  std::unique_ptr<ControlServer> controlServer;
  pw_main_loop *mainLoop;
  pw_context *context;
  pw_core *core;
  pw_stream *inputStream;
  pw_stream *outputStream;
  spa_source *rateChangeSource;
  spa_source *timeoutSource;
  spa_source *interruptSource;
  spa_source *terminateSource;
  pw_stream_events inputEvents;
  pw_stream_events outputEvents;
  spa_hook inputListener;
  spa_hook outputListener;
  StreamCallbackContext inputContext;
  StreamCallbackContext outputContext;
  bool inputListenerInstalled;
  bool outputListenerInstalled;
  bool inputReady;
  bool outputReady;
  bool inputFormatReady;
  bool outputFormatReady;
  bool inputAlwaysProcess;
  bool readyNotified;
  bool completed;
  std::string error;

  PipeWireRuntime(std::unique_ptr<DspPipeline> preparedPipeline,
                  const PipeWirePipelineOptions &runtimeOptions,
                  PipeWireRunMode runtimeMode)
      : pipeline(std::move(preparedPipeline)), options(runtimeOptions),
        mode(runtimeMode),
        ring(runtimeOptions.channelCount, runtimeOptions.ringCapacityFrames),
        outputTransitionSilencer(0),
        inputRateConverter(runtimeOptions.channelCount,
                           runtimeOptions.maxFrames),
        outputRateConverter(runtimeOptions.channelCount,
                            runtimeOptions.maxFrames),
        inputScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                         runtimeOptions.maxFrames,
                     0.0F),
        dspScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                       runtimeOptions.maxFrames,
                   0.0F),
        convertedScratch(
            static_cast<std::size_t>(runtimeOptions.channelCount) *
                runtimeOptions.maxFrames,
            0.0F),
        outputScratch(static_cast<std::size_t>(runtimeOptions.channelCount) *
                          runtimeOptions.maxFrames,
                      0.0F),
        processingErrors(0), inputTelemetry(),
        rateBridgeReady(false), inputFormatNegotiated(false),
        dspSampleRate(runtimeOptions.dspSampleRate),
        inputStreamSampleRate(0), outputStreamSampleRate(0),
        graphSampleRate(0), rateBridgeStreamRate(0), rateBridgeDspRate(0),
        silenceNextRateBridge(false), processedInputFrames(0),
        processingMode(runtimeOptions.initialPresetPath.empty()
                           ? ProcessingMode::bypass
                           : ProcessingMode::preset),
        activePreset(runtimeOptions.initialPresetPath.string()),
        configurationError(runtimeOptions.initialConfigurationError),
        configurationRevision(0), presetReloadPending(false),
        dspBackendState(makeDspBackendRuntimeState(
            resolveRuntimeBackends(runtimeOptions),
            runtimeOptions.configuredDspBackend,
            runtimeOptions.configuredDspSimdVariant)),
        pipelineMutationMutex(), dspBackendStateMutex(), rateStateMutex(),
        rateRequestMutex(), rateRequestCondition(),
        configuredRatePolicy(runtimeOptions.ratePolicy),
        pendingRatePolicy(defaultSampleRatePolicy()),
        rateTransitioning(
            runtimeOptions.ratePolicy.mode == SampleRateMode::fixed &&
            runtimeOptions.ratePolicy.fixedRate != runtimeOptions.dspSampleRate),
        rateRequestPending(false), rateRequestCompleted(false),
        rateRequestError(), rateError(), presetFileMonitor(), controlServer(),
        mainLoop(nullptr),
        context(nullptr), core(nullptr), inputStream(nullptr),
        outputStream(nullptr), rateChangeSource(nullptr), timeoutSource(nullptr),
        interruptSource(nullptr), terminateSource(nullptr), inputEvents{},
        outputEvents{}, inputListener{}, outputListener{},
        inputContext{this, true}, outputContext{this, false},
        inputListenerInstalled(false), outputListenerInstalled(false),
        inputReady(false), outputReady(false), inputFormatReady(false),
        outputFormatReady(false), inputAlwaysProcess(true),
        readyNotified(false), completed(false), error() {
    if (processingMode == ProcessingMode::preset &&
        (!dspBackendState.effectiveBackend.has_value() ||
         !dspBackendState.effectiveVariant.has_value() ||
         pipeline.backendVariant() != dspBackendState.effectiveVariant)) {
      throw std::invalid_argument(
          "initial preset pipeline does not match the effective DSP backend");
    }
  }

  ~PipeWireRuntime() {
    controlServer.reset();
    presetFileMonitor.reset();
    if (outputStream != nullptr) {
      if (outputListenerInstalled) {
        spa_hook_remove(&outputListener);
      }
      pw_stream_destroy(outputStream);
    }
    if (inputStream != nullptr) {
      if (inputListenerInstalled) {
        spa_hook_remove(&inputListener);
      }
      pw_stream_destroy(inputStream);
    }
    if (core != nullptr) {
      pw_core_disconnect(core);
    }
    if (context != nullptr) {
      pw_context_destroy(context);
    }
    if (mainLoop != nullptr) {
      auto *loop = pw_main_loop_get_loop(mainLoop);
      if (rateChangeSource != nullptr) {
        pw_loop_destroy_source(loop, rateChangeSource);
      }
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

static std::uint32_t transitionSampleRate(
    const PipeWireRuntime &runtime) noexcept {
  const auto outputRate =
      runtime.outputStreamSampleRate.load(std::memory_order_acquire);
  if (outputRate != 0) {
    return outputRate;
  }
  const auto inputRate =
      runtime.inputStreamSampleRate.load(std::memory_order_acquire);
  return inputRate == 0 ? runtime.rateBridgeStreamRate : inputRate;
}

static void startOutputTransition(PipeWireRuntime &runtime,
                                  std::uint32_t sampleRate) noexcept {
  // Both stream callbacks run on this runtime's pw_main_loop, so the ring
  // consumer cannot run concurrently with this intentional discard.
  static_cast<void>(runtime.ring.discardQueuedFrames());
  runtime.outputTransitionSilencer.start(
      pipelineTransitionSilenceFrames(sampleRate),
      pipelineTransitionFadeFrames(sampleRate));
}

static void resetOutputTransition(PipeWireRuntime &runtime,
                                  std::uint32_t sampleRate) noexcept {
  // Output callbacks share the pw_main_loop with format callbacks, so the
  // consumer cannot run while its disconnected queue is discarded.
  static_cast<void>(runtime.ring.discardQueuedFrames());
  runtime.outputTransitionSilencer.reset(
      pipelineTransitionSilenceFrames(sampleRate),
      pipelineTransitionFadeFrames(sampleRate));
}

struct PipeWireLibraryScope {
  PipeWireLibraryScope() { pw_init(nullptr, nullptr); }
  ~PipeWireLibraryScope() { pw_deinit(); }
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

static void requestControlStatusUpdate(PipeWireRuntime &runtime) {
  publishControlStatus(runtime.controlServer.get());
}

static void completePendingRateRequest(PipeWireRuntime &runtime,
                                       std::string error) {
  {
    auto lock = std::scoped_lock(runtime.rateRequestMutex);
    if (!runtime.rateRequestPending || runtime.rateRequestCompleted) {
      return;
    }
    runtime.rateRequestError = std::move(error);
    runtime.rateRequestCompleted = true;
  }
  runtime.rateRequestCondition.notify_all();
}

static void failRuntime(PipeWireRuntime &runtime, std::string message) {
  if (!runtime.error.empty()) {
    return;
  }
  runtime.error = std::move(message);
  completePendingRateRequest(runtime, runtime.error);
  if (runtime.mainLoop != nullptr) {
    pw_main_loop_quit(runtime.mainLoop);
  }
}

static void completeRuntime(PipeWireRuntime &runtime) {
  if (runtime.completed) {
    return;
  }
  runtime.completed = true;
  completePendingRateRequest(runtime, "PipeTune daemon stopped");
  pw_main_loop_quit(runtime.mainLoop);
}

static bool isReadyState(pw_stream_state state) noexcept {
  return state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
}

static bool setInputAlwaysProcess(PipeWireRuntime &runtime, bool enabled) {
  if (runtime.inputAlwaysProcess == enabled || runtime.inputStream == nullptr) {
    return true;
  }
  auto *properties = pw_properties_new(
      PW_KEY_NODE_ALWAYS_PROCESS, enabled ? "true" : "false", nullptr);
  if (properties == nullptr) {
    failRuntime(runtime, "cannot allocate PipeWire scheduling properties");
    return false;
  }
  const auto result =
      pw_stream_update_properties(runtime.inputStream, &properties->dict);
  pw_properties_free(properties);
  if (result < 0) {
    failRuntime(runtime,
                systemError("cannot update PipeWire scheduling", result));
    return false;
  }
  runtime.inputAlwaysProcess = enabled;
  return true;
}

static void finishReadinessCheck(PipeWireRuntime &runtime) {
  if (!runtime.inputReady || !runtime.outputReady ||
      !runtime.inputFormatReady || !runtime.outputFormatReady ||
      !runtime.inputFormatNegotiated.load(std::memory_order_acquire) ||
      !runtime.rateBridgeReady.load(std::memory_order_acquire)) {
    return;
  }
  const auto inputRate =
      runtime.inputStreamSampleRate.load(std::memory_order_acquire);
  const auto outputRate =
      runtime.outputStreamSampleRate.load(std::memory_order_acquire);
  if (inputRate == 0 || inputRate != outputRate) {
    return;
  }
  if (!setInputAlwaysProcess(runtime, false)) {
    return;
  }

  auto completedRateChange = false;
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    if (runtime.rateTransitioning) {
      runtime.rateTransitioning = false;
      completedRateChange = true;
    }
  }
  if (completedRateChange) {
    completePendingRateRequest(runtime, {});
    requestControlStatusUpdate(runtime);
  }

  if (!runtime.readyNotified) {
    runtime.readyNotified = true;
    if (runtime.options.readyCallback != nullptr) {
      runtime.options.readyCallback(runtime.options.readyUserData);
    }
  }
  if (runtime.mode == PipeWireRunMode::untilReady) {
    completeRuntime(runtime);
  }
}

static void streamStateChanged(void *data, pw_stream_state previousState,
                               pw_stream_state state, const char *error) {
  auto &context = *static_cast<StreamCallbackContext *>(data);
  auto &runtime = *context.runtime;
  if (state == PW_STREAM_STATE_ERROR) {
    const auto detail =
        error == nullptr ? std::string("unknown PipeWire stream error")
                         : std::string(error);
    if (context.input) {
      runtime.inputReady = false;
      runtime.inputFormatReady = false;
      runtime.inputFormatNegotiated.store(false, std::memory_order_release);
      failRuntime(runtime, "filter input: " + detail);
    } else {
      runtime.outputReady = false;
      runtime.outputFormatReady = false;
      requestControlStatusUpdate(runtime);
    }
    return;
  }
  if (pipeWireStateTransitionInvalidatesQueuedAudio(previousState, state)) {
    // PipeWire keeps already queued buffers when a graph is suspended. They
    // belong to the previous source activation and must not play on resume.
    auto *stream = context.input ? runtime.inputStream : runtime.outputStream;
    if (stream != nullptr && pw_stream_flush(stream, false) < 0) {
      runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    }
    resetOutputTransition(runtime, transitionSampleRate(runtime));
  }
  if (context.input) {
    runtime.inputReady = isReadyState(state);
    runtime.inputFormatNegotiated.store(
        runtime.inputReady && runtime.inputFormatReady,
        std::memory_order_release);
  } else {
    runtime.outputReady = isReadyState(state);
  }
  requestControlStatusUpdate(runtime);
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
      SPA_PARAM_BUFFERS_blocks,
      SPA_POD_Int(static_cast<int>(options.channelCount)),
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

static spa_pod *buildAutomaticFormatParameter(
    spa_pod_builder &builder, const PipeWirePipelineOptions &options,
    std::uint32_t preferredRate) {
  auto info = makeRawFormat(options, preferredRate);
  auto frame = spa_pod_frame{};
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      &builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_AUDIO_format, SPA_POD_Id(SPA_AUDIO_FORMAT_F32P),
      SPA_FORMAT_AUDIO_rate,
      SPA_POD_CHOICE_RANGE_Int(static_cast<int>(preferredRate),
                               static_cast<int>(kMinimumGraphSampleRate),
                               static_cast<int>(kMaximumGraphSampleRate)),
      SPA_FORMAT_AUDIO_channels,
      SPA_POD_Int(static_cast<int>(options.channelCount)),
      SPA_FORMAT_AUDIO_position,
      SPA_POD_Array(sizeof(std::uint32_t), SPA_TYPE_Id, info.channels,
                    info.position),
      0);
  return static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame));
}

static bool applyNegotiatedStreamRate(PipeWireRuntime &runtime, bool input,
                                      std::uint32_t negotiatedRate) {
  if (negotiatedRate < kMinimumGraphSampleRate ||
      negotiatedRate > kMaximumGraphSampleRate) {
    failRuntime(runtime, "PipeWire negotiated an unsupported stream rate");
    return false;
  }
  auto policy = SampleRatePolicy{};
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    policy = runtime.configuredRatePolicy;
  }
  const auto bridgeRates =
      resolveSampleRateBridgeRates(policy, negotiatedRate);
  if (bridgeRates.streamSampleRate == 0 || bridgeRates.dspSampleRate == 0) {
    failRuntime(runtime, "cannot resolve PipeWire and DSP sample rates");
    return false;
  }
  const auto otherStreamRate =
      input ? runtime.outputStreamSampleRate.load(std::memory_order_acquire)
            : runtime.inputStreamSampleRate.load(std::memory_order_acquire);
  if (otherStreamRate != 0 && otherStreamRate != negotiatedRate) {
    failRuntime(runtime,
                "PipeWire negotiated different rates for the filter nodes");
    return false;
  }
  const auto currentRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  if (currentRate != bridgeRates.dspSampleRate) {
    auto pipelineLock =
        std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
    auto rebuilt = runtime.pipeline.rebuildActive(
        {.sampleRate = static_cast<float>(bridgeRates.dspSampleRate),
         .maxChannels = runtime.options.channelCount,
         .maxFrames = runtime.options.maxFrames});
    if (rebuilt.pipeline == nullptr) {
      {
        auto lock = std::scoped_lock(runtime.rateStateMutex);
        runtime.rateTransitioning = false;
        runtime.rateError = rebuilt.error;
      }
      completePendingRateRequest(runtime, rebuilt.error);
      failRuntime(runtime, rebuilt.error);
      return false;
    }
    runtime.pipeline.replace(std::move(rebuilt.pipeline));
    runtime.dspSampleRate.store(bridgeRates.dspSampleRate,
                                std::memory_order_release);
    runtime.processedInputFrames = 0;
  }
  if (input) {
    runtime.inputStreamSampleRate.store(negotiatedRate,
                                        std::memory_order_release);
  } else {
    runtime.outputStreamSampleRate.store(negotiatedRate,
                                         std::memory_order_release);
  }
  if (input) {
    runtime.rateBridgeReady.store(false, std::memory_order_release);
    const auto inputError = runtime.inputRateConverter.configure(
        bridgeRates.streamSampleRate, bridgeRates.dspSampleRate);
    const auto outputError = runtime.outputRateConverter.configure(
        bridgeRates.dspSampleRate, bridgeRates.streamSampleRate);
    if (inputError != 0 || outputError != 0) {
      failRuntime(runtime,
                  "cannot configure PipeWire-to-DSP sample-rate conversion");
      return false;
    }
    const auto bridgeChanged =
        runtime.rateBridgeStreamRate != 0 &&
        (runtime.rateBridgeStreamRate != bridgeRates.streamSampleRate ||
         runtime.rateBridgeDspRate != bridgeRates.dspSampleRate);
    if (bridgeChanged || runtime.silenceNextRateBridge) {
      startOutputTransition(runtime, bridgeRates.streamSampleRate);
    }
    runtime.rateBridgeStreamRate = bridgeRates.streamSampleRate;
    runtime.rateBridgeDspRate = bridgeRates.dspSampleRate;
    runtime.silenceNextRateBridge = false;
    runtime.rateBridgeReady.store(true, std::memory_order_release);
  } else if (runtime.rateBridgeStreamRate == bridgeRates.streamSampleRate &&
             runtime.rateBridgeDspRate == bridgeRates.dspSampleRate) {
    runtime.rateBridgeReady.store(true, std::memory_order_release);
  }
  return true;
}

static void streamParameterChanged(void *data, std::uint32_t id,
                                   const spa_pod *parameter) {
  if (id != SPA_PARAM_Format) {
    return;
  }
  auto &context = *static_cast<StreamCallbackContext *>(data);
  auto &runtime = *context.runtime;
  if (parameter == nullptr) {
    const auto sampleRate = transitionSampleRate(runtime);
    if (context.input) {
      startOutputTransition(runtime, sampleRate);
    } else {
      resetOutputTransition(runtime, sampleRate);
    }
    runtime.silenceNextRateBridge = true;
    runtime.rateBridgeReady.store(false, std::memory_order_release);
    if (context.input) {
      runtime.inputFormatReady = false;
      runtime.inputStreamSampleRate.store(0, std::memory_order_release);
      runtime.inputFormatNegotiated.store(false,
                                          std::memory_order_release);
    } else {
      runtime.outputFormatReady = false;
      runtime.outputStreamSampleRate.store(0, std::memory_order_release);
    }
    runtime.graphSampleRate.store(0, std::memory_order_release);
    requestControlStatusUpdate(runtime);
    return;
  }
  auto negotiated = spa_audio_info_raw{};
  const auto parseResult = spa_format_audio_raw_parse(parameter, &negotiated);
  const auto previousRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  if (parseResult < 0 || negotiated.format != SPA_AUDIO_FORMAT_F32P ||
      negotiated.channels != runtime.options.channelCount ||
      !applyNegotiatedStreamRate(runtime, context.input, negotiated.rate)) {
    if (!runtime.error.empty()) {
      return;
    }
    failRuntime(runtime, "PipeWire negotiated an unsupported audio format");
    return;
  }
  const auto currentRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  if (previousRate != currentRate) {
    if (context.input) {
      runtime.outputFormatReady = false;
      runtime.outputStreamSampleRate.store(0, std::memory_order_release);
    } else {
      runtime.inputFormatReady = false;
      runtime.inputStreamSampleRate.store(0, std::memory_order_release);
      runtime.inputFormatNegotiated.store(false,
                                          std::memory_order_release);
    }
  }

  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const spa_pod *parameters[] = {
      buildBufferParameter(builder, runtime.options)};
  auto *stream = context.input ? runtime.inputStream : runtime.outputStream;
  const auto updateResult = pw_stream_update_params(stream, parameters, 1);
  if (updateResult < 0) {
    failRuntime(runtime,
                systemError("cannot configure PipeWire buffers", updateResult));
    return;
  }
  if (context.input) {
    runtime.inputFormatReady = true;
    runtime.inputFormatNegotiated.store(runtime.inputReady,
                                        std::memory_order_release);
  } else {
    runtime.outputFormatReady = true;
  }
  requestControlStatusUpdate(runtime);
  finishReadinessCheck(runtime);
}

static void copyInputPlane(const spa_data &plane, std::uint32_t sourceFrame,
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

static void updateGraphSampleRate(PipeWireRuntime &runtime,
                                  pw_stream *stream) noexcept {
  auto time = pw_time{};
  if (pw_stream_get_time_n(stream, &time, sizeof(time)) < 0) {
    return;
  }
  const auto sampleRate = pipeWireGraphSampleRate(time.rate);
  if (sampleRate >= kMinimumGraphSampleRate &&
      sampleRate <= kMaximumGraphSampleRate) {
    runtime.graphSampleRate.store(sampleRate, std::memory_order_release);
  }
}

static bool convertProcessedDspFrames(
    PipeWireRuntime &runtime, std::span<const float> dspSamples,
    std::uint32_t dspFrameCount, std::uint64_t generation) noexcept {
  auto dspFrameOffset = std::uint32_t{0};
  while (dspFrameOffset < dspFrameCount) {
    auto convertedScratch = std::span<float>(runtime.convertedScratch);
    const auto converted = runtime.outputRateConverter.process(
        dspSamples, dspFrameCount, dspFrameOffset,
        dspFrameCount - dspFrameOffset, convertedScratch,
        runtime.options.maxFrames);
    if (converted.error != 0 ||
        (converted.inputFramesUsed == 0 &&
         converted.outputFramesGenerated == 0)) {
      return false;
    }
    dspFrameOffset += converted.inputFramesUsed;
    if (converted.outputFramesGenerated == 0) {
      continue;
    }
    convertedScratch = convertedScratch.first(
        static_cast<std::size_t>(runtime.options.channelCount) *
        converted.outputFramesGenerated);
    runtime.ring.write(convertedScratch,
                       converted.outputFramesGenerated, generation);
  }
  return true;
}

static bool processStreamInputBlock(PipeWireRuntime &runtime,
                                    std::span<const float> streamSamples,
                                    std::uint32_t streamFrameCount) noexcept {
  auto inputFrameOffset = std::uint32_t{0};
  while (inputFrameOffset < streamFrameCount) {
    auto dspScratch = std::span<float>(runtime.dspScratch);
    const auto converted = runtime.inputRateConverter.process(
        streamSamples, streamFrameCount, inputFrameOffset,
        streamFrameCount - inputFrameOffset, dspScratch,
        runtime.options.maxFrames);
    if (converted.error != 0 ||
        (converted.inputFramesUsed == 0 &&
         converted.outputFramesGenerated == 0)) {
      return false;
    }
    inputFrameOffset += converted.inputFramesUsed;
    const auto dspFrameCount = converted.outputFramesGenerated;
    if (dspFrameCount == 0) {
      continue;
    }
    dspScratch = dspScratch.first(
        static_cast<std::size_t>(runtime.options.channelCount) *
        dspFrameCount);
    const auto sampleRate =
        runtime.dspSampleRate.load(std::memory_order_relaxed);
    const auto timeSeconds =
        static_cast<double>(runtime.processedInputFrames) / sampleRate;
    const auto processed = runtime.pipeline.processWithGeneration(
        dspScratch, runtime.options.channelCount, dspFrameCount,
        timeSeconds);
    if (processed.status != ProcessStatus::ok) {
      runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    }
    runtime.processedInputFrames += dspFrameCount;
    if (!convertProcessedDspFrames(runtime, dspScratch, dspFrameCount,
                                   processed.generation)) {
      return false;
    }
  }
  return true;
}

static void inputProcess(void *data) {
  auto &runtime =
      *static_cast<StreamCallbackContext *>(data)->runtime;
  updateGraphSampleRate(runtime, runtime.inputStream);
  auto *pipeWireBuffer = pw_stream_dequeue_buffer(runtime.inputStream);
  if (pipeWireBuffer == nullptr || pipeWireBuffer->buffer == nullptr) {
    return;
  }
  auto &buffer = *pipeWireBuffer->buffer;
  auto frameCount = std::uint32_t{0};
  if (!inspectPipeWireCaptureBuffer(buffer, runtime.options.channelCount,
                                    frameCount)) {
    runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    pipeWireBuffer->size = 0;
    retirePipeWireCaptureBuffer(buffer);
    pw_stream_queue_buffer(runtime.inputStream, pipeWireBuffer);
    return;
  }
  if (frameCount != 0) {
    recordInputFrames(runtime.inputTelemetry, frameCount,
                      currentMonotonicNanoseconds());
  }

  if (!runtime.rateBridgeReady.load(std::memory_order_acquire)) {
    pipeWireBuffer->size = frameCount;
    retirePipeWireCaptureBuffer(buffer);
    pw_stream_queue_buffer(runtime.inputStream, pipeWireBuffer);
    return;
  }

  auto sourceFrame = std::uint32_t{0};
  while (sourceFrame < frameCount) {
    const auto blockFrames =
        std::min(runtime.options.maxFrames, frameCount - sourceFrame);
    auto scratch = std::span<float>(runtime.inputScratch)
                       .first(static_cast<std::size_t>(
                                  runtime.options.channelCount) *
                              blockFrames);
    for (auto channel = std::uint32_t{0};
         channel < runtime.options.channelCount; ++channel) {
      auto channelScratch = scratch.subspan(
          static_cast<std::size_t>(channel) * blockFrames, blockFrames);
      copyInputPlane(buffer.datas[channel], sourceFrame, channelScratch);
    }
    if (!processStreamInputBlock(runtime, scratch, blockFrames)) {
      runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    }
    sourceFrame += blockFrames;
  }

  pipeWireBuffer->size = frameCount;
  retirePipeWireCaptureBuffer(buffer);
  pw_stream_queue_buffer(runtime.inputStream, pipeWireBuffer);
}

static bool inspectOutputBuffer(const spa_buffer &buffer,
                                std::uint32_t channelCount,
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
    capacityFrames = std::min(capacityFrames,
                              plane.maxsize / kSampleBytes);
  }
  return capacityFrames != UINT32_MAX;
}

static void clearOutputChunks(spa_buffer &buffer,
                              std::uint32_t channelCount) noexcept {
  const auto availableChannels = std::min(buffer.n_datas, channelCount);
  for (auto channel = std::uint32_t{0}; channel < availableChannels;
       ++channel) {
    auto &plane = buffer.datas[channel];
    if (plane.chunk != nullptr) {
      plane.chunk->offset = 0;
      plane.chunk->size = 0;
      plane.chunk->stride = static_cast<std::int32_t>(kSampleBytes);
      plane.chunk->flags = SPA_CHUNK_FLAG_EMPTY;
    }
  }
}

static void outputProcess(void *data) {
  auto &runtime =
      *static_cast<StreamCallbackContext *>(data)->runtime;
  updateGraphSampleRate(runtime, runtime.outputStream);
  auto *pipeWireBuffer = pw_stream_dequeue_buffer(runtime.outputStream);
  if (pipeWireBuffer == nullptr || pipeWireBuffer->buffer == nullptr) {
    return;
  }
  auto &buffer = *pipeWireBuffer->buffer;
  auto capacityFrames = std::uint32_t{0};
  if (!inspectOutputBuffer(buffer, runtime.options.channelCount,
                           capacityFrames)) {
    runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    clearOutputChunks(buffer, runtime.options.channelCount);
    pipeWireBuffer->size = 0;
    pw_stream_queue_buffer(runtime.outputStream, pipeWireBuffer);
    return;
  }

  const auto suggestedFrames =
      pipeWireBuffer->requested == 0
          ? capacityFrames
          : static_cast<std::uint32_t>(std::min<std::uint64_t>(
                pipeWireBuffer->requested, capacityFrames));
  auto outputFrame = std::uint32_t{0};
  while (outputFrame < suggestedFrames) {
    const auto blockFrames =
        std::min(runtime.options.maxFrames, suggestedFrames - outputFrame);
    auto scratch = std::span<float>(runtime.outputScratch)
                       .first(static_cast<std::size_t>(
                                  runtime.options.channelCount) *
                              blockFrames);
    const auto pipelineGeneration = runtime.pipeline.activeGeneration();
    const auto availableFrames =
        runtime.ring.read(scratch, blockFrames, pipelineGeneration);
    const auto outputSampleRate = runtime.outputStreamSampleRate.load(
        std::memory_order_relaxed);
    runtime.outputTransitionSilencer.apply(
        scratch, runtime.options.channelCount, blockFrames, availableFrames,
        pipelineGeneration,
        pipelineTransitionSilenceFrames(outputSampleRate),
        pipelineTransitionFadeFrames(outputSampleRate));
    for (auto channel = std::uint32_t{0};
         channel < runtime.options.channelCount; ++channel) {
      const auto source = scratch.subspan(
          static_cast<std::size_t>(channel) * blockFrames, blockFrames);
      auto *destination = static_cast<float *>(buffer.datas[channel].data);
      std::copy(source.begin(), source.end(), destination + outputFrame);
    }
    outputFrame += blockFrames;
  }

  for (auto channel = std::uint32_t{0};
       channel < runtime.options.channelCount; ++channel) {
    auto &chunk = *buffer.datas[channel].chunk;
    chunk.offset = 0;
    chunk.size = suggestedFrames * kSampleBytes;
    chunk.stride = static_cast<std::int32_t>(kSampleBytes);
    chunk.flags = suggestedFrames == 0 ? SPA_CHUNK_FLAG_EMPTY
                                       : SPA_CHUNK_FLAG_NONE;
  }
  pipeWireBuffer->size = suggestedFrames;
  pw_stream_queue_buffer(runtime.outputStream, pipeWireBuffer);
}

static pw_properties *makePipeWireProperties(
    const FilterNodeProperties &properties) {
  auto *result = pw_properties_new(nullptr, nullptr);
  if (result == nullptr) {
    return nullptr;
  }
  for (const auto &[key, value] : properties) {
    if (pw_properties_set(result, key.c_str(), value.c_str()) < 0) {
      pw_properties_free(result);
      return nullptr;
    }
  }
  return result;
}

static FilterGraphProperties currentFilterGraphProperties(
    PipeWireRuntime &runtime) {
  auto fixedSampleRate = std::optional<std::uint32_t>{};
  auto forceRate = false;
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    if (runtime.configuredRatePolicy.mode == SampleRateMode::fixed) {
      fixedSampleRate = runtime.configuredRatePolicy.fixedRate;
      forceRate = runtime.configuredRatePolicy.enforcement ==
                  SampleRateEnforcement::force;
    }
  }
  return makeFilterGraphProperties(
      {.nodeName = runtime.options.filterName,
       .nodeDescription = runtime.options.filterDescription,
       .fixedSampleRate = fixedSampleRate,
       .channelCount = runtime.options.channelCount,
       .forceRate = forceRate});
}

static std::string connectStream(PipeWireRuntime &runtime, pw_stream *stream,
                                 pw_direction direction, bool autoconnect,
                                 bool reconnect) {
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const auto preferredRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  auto policy = SampleRatePolicy{};
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    policy = runtime.configuredRatePolicy;
  }
  auto info = makeRawFormat(runtime.options, preferredRate);
  const spa_pod *format =
      policy.mode == SampleRateMode::fixed
          ? spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info)
          : buildAutomaticFormatParameter(builder, runtime.options,
                                          preferredRate);
  const spa_pod *parameters[] = {format};
  auto flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (autoconnect) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
  }
  if (!reconnect) {
    flags |= PW_STREAM_FLAG_DONT_RECONNECT;
  }
  const auto result =
      pw_stream_connect(stream, direction, PW_ID_ANY,
                        static_cast<pw_stream_flags>(flags), parameters, 1);
  return result < 0 ? systemError("cannot connect PipeWire stream", result)
                    : std::string{};
}

static void destroyAudioStreams(PipeWireRuntime &runtime) {
  resetOutputTransition(runtime, transitionSampleRate(runtime));
  runtime.rateBridgeReady.store(false, std::memory_order_release);
  if (runtime.outputStream != nullptr) {
    if (runtime.outputListenerInstalled) {
      spa_hook_remove(&runtime.outputListener);
      runtime.outputListenerInstalled = false;
    }
    pw_stream_destroy(runtime.outputStream);
    runtime.outputStream = nullptr;
  }
  if (runtime.inputStream != nullptr) {
    if (runtime.inputListenerInstalled) {
      spa_hook_remove(&runtime.inputListener);
      runtime.inputListenerInstalled = false;
    }
    pw_stream_destroy(runtime.inputStream);
    runtime.inputStream = nullptr;
  }
  runtime.inputReady = false;
  runtime.outputReady = false;
  runtime.inputFormatReady = false;
  runtime.outputFormatReady = false;
  runtime.inputStreamSampleRate.store(0, std::memory_order_release);
  runtime.outputStreamSampleRate.store(0, std::memory_order_release);
  runtime.graphSampleRate.store(0, std::memory_order_release);
  runtime.inputAlwaysProcess = true;
  runtime.inputFormatNegotiated.store(false, std::memory_order_release);
}

static std::string createAudioStreams(PipeWireRuntime &runtime) {
  const auto graph = currentFilterGraphProperties(runtime);
  auto *inputProperties = makePipeWireProperties(graph.input);
  if (inputProperties == nullptr) {
    return "cannot allocate PipeWire filter-input properties";
  }
  runtime.inputStream =
      pw_stream_new(runtime.core, "PipeTune filter input", inputProperties);
  if (runtime.inputStream == nullptr) {
    return systemError("cannot create PipeWire filter input", -errno);
  }
  runtime.inputListener = {};
  pw_stream_add_listener(runtime.inputStream, &runtime.inputListener,
                         &runtime.inputEvents, &runtime.inputContext);
  runtime.inputListenerInstalled = true;
  const auto inputError =
      connectStream(runtime, runtime.inputStream, PW_DIRECTION_INPUT,
                    graph.inputAutoconnect, graph.inputReconnect);
  if (!inputError.empty()) {
    destroyAudioStreams(runtime);
    return inputError;
  }

  auto *outputProperties = makePipeWireProperties(graph.output);
  if (outputProperties == nullptr) {
    destroyAudioStreams(runtime);
    return "cannot allocate PipeWire filter-output properties";
  }
  runtime.outputStream =
      pw_stream_new(runtime.core, "PipeTune filter output", outputProperties);
  if (runtime.outputStream == nullptr) {
    destroyAudioStreams(runtime);
    return systemError("cannot create PipeWire filter output", -errno);
  }
  runtime.outputListener = {};
  pw_stream_add_listener(runtime.outputStream, &runtime.outputListener,
                         &runtime.outputEvents, &runtime.outputContext);
  runtime.outputListenerInstalled = true;
  const auto outputError =
      connectStream(runtime, runtime.outputStream, PW_DIRECTION_OUTPUT,
                    graph.outputAutoconnect, graph.outputReconnect);
  if (!outputError.empty()) {
    destroyAudioStreams(runtime);
    return outputError;
  }
  return {};
}

static ControlDspBackendAvailability controlDspBackendAvailability(
    DspBackendKind kind, const DspBackendLoadResult &result) {
  auto cpuRequirement = result.cpuRequirement;
  if (cpuRequirement.empty()) {
    cpuRequirement = kind == DspBackendKind::scalar ? "none" : "unknown";
  }
  auto error = result.error;
  if (result.backend == nullptr && error.empty()) {
    error = std::string(dspBackendName(kind)) +
            " DSP backend is unavailable";
  }
  return {.kind = kind,
          .available = result.backend != nullptr,
          .cpuRequirement = std::move(cpuRequirement),
          .error = std::move(error)};
}

static ControlDspVariantAvailability controlDspVariantAvailability(
    const DspBackendLoadResult &result) {
  auto cpuRequirement = result.cpuRequirement;
  if (cpuRequirement.empty()) {
    cpuRequirement = result.variant == DspBackendVariant::scalar
                         ? "none"
                         : "unknown";
  }
  auto error = result.error;
  if (result.backend == nullptr && error.empty()) {
    error = "DSP variant " +
            std::string(dspBackendVariantName(result.variant)) +
            " is unavailable";
  }
  return {.variant = result.variant,
          .available = result.backend != nullptr,
          .cpuSupported = result.cpuSupported,
          .cpuRequirement = std::move(cpuRequirement),
          .error = std::move(error)};
}

static ControlRuntimeStatus controlStatus(PipeWireRuntime &runtime) {
  const auto input = snapshotInputTelemetry(
      runtime.inputTelemetry, currentMonotonicNanoseconds(),
      currentUnixMilliseconds());
  const auto inputNegotiated =
      runtime.inputFormatNegotiated.load(std::memory_order_acquire);
  const auto inputSampleRate =
      runtime.inputStreamSampleRate.load(std::memory_order_acquire);
  const auto dspPerformance = runtime.pipeline.performanceCounters();
  auto ratePolicy = SampleRatePolicy{};
  auto rateTransitioning = false;
  auto rateError = std::string{};
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    ratePolicy = runtime.configuredRatePolicy;
    rateTransitioning = runtime.rateTransitioning;
    rateError = runtime.rateError;
  }
  auto configuredBackend = DspBackendKind::scalar;
  auto configuredVariant = DspSimdVariant::automatic;
  auto effectiveBackend = std::optional<DspBackendKind>{};
  auto effectiveVariant = std::optional<DspBackendVariant>{};
  auto backendFallback = false;
  auto backendError = std::string{};
  auto availableBackends =
      std::array<ControlDspBackendAvailability, 2>{};
  auto availableVariants = std::vector<ControlDspVariantAvailability>{};
  {
    auto lock = std::scoped_lock(runtime.dspBackendStateMutex);
    configuredBackend = runtime.dspBackendState.configuredBackend;
    configuredVariant = runtime.dspBackendState.configuredSimdVariant;
    effectiveBackend = runtime.dspBackendState.effectiveBackend;
    effectiveVariant = runtime.dspBackendState.effectiveVariant;
    backendFallback = runtime.dspBackendState.fallback;
    backendError = runtime.dspBackendState.error;
    availableBackends = {
        controlDspBackendAvailability(DspBackendKind::scalar,
                                      runtime.dspBackendState.backends.scalar),
        controlDspBackendAvailability(DspBackendKind::simd,
                                      runtime.dspBackendState.backends.simd)};
    availableVariants.reserve(
        runtime.dspBackendState.backends.simdVariants.size() + 1);
    availableVariants.push_back(controlDspVariantAvailability(
        runtime.dspBackendState.backends.scalar));
    for (const auto &variant :
         runtime.dspBackendState.backends.simdVariants) {
      availableVariants.push_back(controlDspVariantAvailability(variant));
    }
  }
  const auto sampleRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  const auto graphSampleRate =
      runtime.graphSampleRate.load(std::memory_order_acquire);
  if (!rateTransitioning &&
      ratePolicy.mode == SampleRateMode::fixed &&
      ratePolicy.enforcement == SampleRateEnforcement::force &&
      graphSampleRate != 0 && graphSampleRate != ratePolicy.fixedRate &&
      rateError.empty()) {
    rateError = "PipeWire graph is " +
                std::to_string(graphSampleRate) +
                " Hz instead of the forced " +
                std::to_string(ratePolicy.fixedRate) + " Hz";
  }
  return {.processingMode = runtime.processingMode,
          .activePreset = runtime.activePreset,
          .configurationError = runtime.configurationError,
          .configurationRevision = runtime.configurationRevision.load(
              std::memory_order_acquire),
          .activePluginCount = runtime.pipeline.activePluginCount(),
          .overrunFrames = runtime.ring.overrunFrames(),
          .underrunFrames = runtime.ring.underrunFrames(),
          .processingErrors =
              runtime.processingErrors.load(std::memory_order_relaxed),
          .dspProcessedFrames = dspPerformance.processedFrames,
          .dspProcessingNanoseconds = dspPerformance.processingNanoseconds,
          .inputSampleFormat = inputNegotiated ? "F32P" : "",
          .inputSampleRate = inputNegotiated ? inputSampleRate : 0,
          .inputChannelCount =
              inputNegotiated ? runtime.options.channelCount : 0,
          .inputFramesReceived = input.framesReceived,
          .inputLastReceivedUnixMilliseconds =
              input.lastReceivedUnixMilliseconds,
          .configuredRatePolicy = ratePolicy,
          .dspSampleRate = sampleRate,
          .graphSampleRate = graphSampleRate,
          .rateTransitioning = rateTransitioning,
          .rateError = std::move(rateError),
          .configuredDspBackend = configuredBackend,
          .configuredDspSimdVariant = configuredVariant,
          .effectiveDspBackend = effectiveBackend,
          .effectiveDspVariant = effectiveVariant,
          .dspBackendFallback = backendFallback,
          .dspBackendError = std::move(backendError),
          .availableDspBackends = std::move(availableBackends),
          .availableDspVariants = std::move(availableVariants)};
}

static ControlMessageResult closeControlResponse(std::string response,
                                                 bool publishStatus) {
  return {.response = std::move(response),
          .connectionMode = ControlConnectionMode::close,
          .publishStatus = publishStatus};
}

struct PresetActivationResult {
  std::vector<ControlWarning> warnings;
  std::string error;
  bool deferred;
};

static PresetActivationResult activatePreset(
    PipeWireRuntime &runtime, const std::filesystem::path &presetPath,
    bool automaticReload) {
  auto pipelineLock =
      std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
  {
    auto rateLock = std::scoped_lock(runtime.rateStateMutex);
    if (runtime.rateTransitioning) {
      return {
          .warnings = {},
          .error = automaticReload
                       ? std::string{}
                       : "cannot load a preset during sample-rate transition",
          .deferred = automaticReload,
      };
    }
  }

  auto backend = std::shared_ptr<const DspBackend>{};
  {
    auto backendLock = std::scoped_lock(runtime.dspBackendStateMutex);
    if (runtime.dspBackendState.effectiveVariant.has_value()) {
      const auto *loaded = runtime.dspBackendState.backends.find(
          *runtime.dspBackendState.effectiveVariant);
      if (loaded != nullptr) {
        backend = loaded->backend;
      }
    }
  }
  if (backend == nullptr) {
    const auto error =
        "cannot load a preset without a usable scalar DSP backend";
    if (automaticReload) {
      runtime.configurationError =
          "Automatic preset reload failed: " + std::string(error);
    }
    return {.warnings = {}, .error = error, .deferred = false};
  }

  auto loaded = loadDspPipeline(
      presetPath,
      {.sampleRate = static_cast<float>(
           runtime.dspSampleRate.load(std::memory_order_acquire)),
       .maxChannels = runtime.options.channelCount,
       .maxFrames = runtime.options.maxFrames},
      std::move(backend));
  if (loaded.pipeline == nullptr) {
    if (automaticReload) {
      runtime.configurationError =
          "Automatic preset reload failed: " + loaded.error;
    }
    return {.warnings = {},
            .error = std::move(loaded.error),
            .deferred = false};
  }

  auto warnings = std::vector<ControlWarning>{};
  warnings.reserve(loaded.warnings.size());
  for (auto &warning : loaded.warnings) {
    warnings.push_back({.nodeIndex = warning.nodeIndex,
                        .pluginName = std::move(warning.pluginName),
                        .reason = std::move(warning.reason)});
  }
  runtime.pipeline.replace(std::move(loaded.pipeline));
  runtime.processingMode = ProcessingMode::preset;
  runtime.activePreset = presetPath.string();
  runtime.configurationError.clear();
  runtime.configurationRevision.fetch_add(1, std::memory_order_release);
  runtime.presetReloadPending = false;
  if (!automaticReload && runtime.presetFileMonitor != nullptr) {
    const auto monitorError = runtime.presetFileMonitor->setPath(presetPath);
    if (!monitorError.empty()) {
      runtime.configurationError = monitorError;
    }
  }
  return {.warnings = std::move(warnings), .error = {}, .deferred = false};
}

static bool retryPendingPresetReload(PipeWireRuntime &runtime) {
  if (!runtime.presetReloadPending) {
    return false;
  }
  if (runtime.processingMode != ProcessingMode::preset ||
      runtime.activePreset.empty()) {
    runtime.presetReloadPending = false;
    return false;
  }
  const auto activated = activatePreset(
      runtime, std::filesystem::path(runtime.activePreset), true);
  if (activated.deferred) {
    return false;
  }
  runtime.presetReloadPending = false;
  return true;
}

static bool handlePresetFileMonitorEvent(void *userData) {
  auto &runtime = *static_cast<PipeWireRuntime *>(userData);
  if (runtime.presetFileMonitor == nullptr) {
    return false;
  }
  const auto event = runtime.presetFileMonitor->consume();
  if (!event.error.empty()) {
    runtime.configurationError = event.error;
    return true;
  }
  if (!event.changed || runtime.processingMode != ProcessingMode::preset ||
      runtime.activePreset.empty()) {
    return false;
  }
  const auto activated = activatePreset(
      runtime, std::filesystem::path(runtime.activePreset), true);
  runtime.presetReloadPending = activated.deferred;
  return !activated.deferred;
}

static std::string provideControlStatus(void *userData) {
  auto &runtime = *static_cast<PipeWireRuntime *>(userData);
  static_cast<void>(retryPendingPresetReload(runtime));
  return makeControlStatusEvent(controlStatus(runtime));
}

static std::string requestLiveRateChange(
    PipeWireRuntime &runtime, const SampleRatePolicy &policy) {
  auto lock = std::unique_lock<std::mutex>(runtime.rateRequestMutex);
  if (runtime.rateRequestPending) {
    return "another sample-rate request is already pending";
  }
  runtime.rateRequestPending = true;
  runtime.rateRequestCompleted = false;
  runtime.pendingRatePolicy = policy;
  runtime.rateRequestError.clear();
  const auto result = pw_loop_signal_event(
      pw_main_loop_get_loop(runtime.mainLoop), runtime.rateChangeSource);
  if (result < 0) {
    runtime.rateRequestPending = false;
    return systemError("cannot schedule PipeWire rate change", result);
  }
  runtime.rateRequestCondition.wait(
      lock, [&runtime] { return runtime.rateRequestCompleted; });
  auto error = std::move(runtime.rateRequestError);
  runtime.rateRequestPending = false;
  runtime.rateRequestCompleted = false;
  return error;
}

static ControlMessageResult handleControlRequest(std::string_view message,
                                                 void *userData) {
  auto &runtime = *static_cast<PipeWireRuntime *>(userData);
  const auto request = parseControlRequest(message);
  if (!request.error.empty()) {
    return closeControlResponse(makeControlErrorResponse(request.error), false);
  }
  if (request.request.command == ControlCommand::subscribe) {
    return {.response = provideControlStatus(&runtime),
            .connectionMode = ControlConnectionMode::subscribe,
            .publishStatus = false};
  }
  if (request.request.command == ControlCommand::status) {
    const auto reloaded = retryPendingPresetReload(runtime);
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), {}), reloaded);
  }
  auto warnings = std::vector<ControlWarning>{};
  if (request.request.command == ControlCommand::setRate) {
    const auto error =
        requestLiveRateChange(runtime, request.request.ratePolicy);
    if (!error.empty()) {
      return closeControlResponse(makeControlErrorResponse(error), false);
    }
    static_cast<void>(retryPendingPresetReload(runtime));
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), true);
  }
  if (request.request.command == ControlCommand::setDspBackend) {
    auto switched = DspBackendSwitchResult{};
    {
      auto pipelineLock =
          std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
      auto rateTransitioning = false;
      {
        auto rateLock = std::scoped_lock(runtime.rateStateMutex);
        rateTransitioning = runtime.rateTransitioning;
      }
      auto backendLock =
          std::scoped_lock(runtime.dspBackendStateMutex);
      switched = switchDspBackend(
          runtime.pipeline, runtime.dspBackendState,
          request.request.dspBackend, request.request.dspSimdVariant,
          {.sampleRate = static_cast<float>(
               runtime.dspSampleRate.load(std::memory_order_acquire)),
           .maxChannels = runtime.options.channelCount,
           .maxFrames = runtime.options.maxFrames},
          rateTransitioning);
    }
    if (!switched.error.empty()) {
      return closeControlResponse(makeControlErrorResponse(switched.error),
                                  false);
    }
    if (switched.changed) {
      runtime.configurationRevision.fetch_add(1,
                                              std::memory_order_release);
    }
    for (auto &warning : switched.warnings) {
      warnings.push_back({.nodeIndex = warning.nodeIndex,
                          .pluginName = std::move(warning.pluginName),
                          .reason = std::move(warning.reason)});
    }
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings),
        switched.changed);
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
    runtime.presetReloadPending = false;
    if (runtime.presetFileMonitor != nullptr) {
      runtime.presetFileMonitor->clear();
    }
    runtime.configurationRevision.fetch_add(1,
                                            std::memory_order_release);
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), true);
  }
  if (request.request.command == ControlCommand::loadPreset) {
    auto activated =
        activatePreset(runtime, request.request.presetPath, false);
    if (!activated.error.empty()) {
      return closeControlResponse(
          makeControlErrorResponse(activated.error), false);
    }
    warnings = std::move(activated.warnings);
    return closeControlResponse(
        makeControlSuccessResponse(controlStatus(runtime), warnings), true);
  }
  return closeControlResponse(
      makeControlSuccessResponse(controlStatus(runtime), warnings), false);
}

static void rateChangeRequested(void *data, std::uint64_t) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  auto requested = SampleRatePolicy{};
  {
    auto lock = std::scoped_lock(runtime.rateRequestMutex);
    if (!runtime.rateRequestPending || runtime.rateRequestCompleted) {
      return;
    }
    requested = runtime.pendingRatePolicy;
  }
  if (!sampleRatePolicyIsValid(requested)) {
    completePendingRateRequest(runtime, "sample-rate policy is invalid");
    return;
  }
  auto previous = SampleRatePolicy{};
  auto unchanged = false;
  auto configurationChanged = false;
  const auto currentRate =
      runtime.dspSampleRate.load(std::memory_order_acquire);
  {
    auto lock = std::scoped_lock(runtime.rateStateMutex);
    previous = runtime.configuredRatePolicy;
    if (requested == previous) {
      runtime.rateError.clear();
      unchanged = true;
    } else {
      runtime.configuredRatePolicy = requested;
      runtime.rateError.clear();
      runtime.rateTransitioning = true;
      configurationChanged = true;
    }
  }
  if (configurationChanged) {
    runtime.configurationRevision.fetch_add(1,
                                            std::memory_order_release);
  }
  if (unchanged) {
    completePendingRateRequest(runtime, {});
    requestControlStatusUpdate(runtime);
    return;
  }

  auto pipelineLock =
      std::unique_lock<std::mutex>(runtime.pipelineMutationMutex);
  if (requested.mode == SampleRateMode::fixed &&
      requested.fixedRate != currentRate) {
    auto rebuilt = runtime.pipeline.rebuildActive(
        {.sampleRate = static_cast<float>(requested.fixedRate),
         .maxChannels = runtime.options.channelCount,
         .maxFrames = runtime.options.maxFrames});
    if (rebuilt.pipeline == nullptr) {
      {
        auto lock = std::scoped_lock(runtime.rateStateMutex);
        runtime.configuredRatePolicy = previous;
        runtime.rateTransitioning = false;
        runtime.rateError = rebuilt.error;
      }
      runtime.configurationRevision.fetch_add(1,
                                              std::memory_order_release);
      completePendingRateRequest(runtime, rebuilt.error);
      requestControlStatusUpdate(runtime);
      return;
    }
    runtime.pipeline.replace(std::move(rebuilt.pipeline));
    runtime.dspSampleRate.store(requested.fixedRate,
                                std::memory_order_release);
    runtime.processedInputFrames = 0;
  }

  destroyAudioStreams(runtime);
  runtime.silenceNextRateBridge = true;
  const auto streamError = createAudioStreams(runtime);
  if (!streamError.empty()) {
    {
      auto lock = std::scoped_lock(runtime.rateStateMutex);
      runtime.rateTransitioning = false;
      runtime.rateError = streamError;
    }
    completePendingRateRequest(runtime, streamError);
    failRuntime(runtime, streamError);
  }
}

static bool createControlServer(PipeWireRuntime &runtime) {
  if (runtime.options.controlSocketPath.empty()) {
    return true;
  }
  auto monitored = createActivePresetFileMonitor(
      std::filesystem::path(runtime.activePreset));
  if (monitored.monitor == nullptr) {
    failRuntime(runtime,
                "cannot start active preset monitoring: " +
                    monitored.error);
    return false;
  }
  runtime.presetFileMonitor = std::move(monitored.monitor);
  auto started = startControlServer(
      runtime.options.controlSocketPath,
      {.handler = handleControlRequest,
       .statusProvider = provideControlStatus,
       .userData = &runtime,
       .eventDescriptor = runtime.presetFileMonitor->descriptor(),
       .eventHandler = handlePresetFileMonitorEvent});
  if (started.server == nullptr) {
    runtime.presetFileMonitor.reset();
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
  runtime.rateChangeSource =
      pw_loop_add_event(pw_main_loop_get_loop(runtime.mainLoop),
                        rateChangeRequested, &runtime);
  if (runtime.rateChangeSource == nullptr) {
    failRuntime(runtime,
                systemError("cannot create PipeWire rate-change event", -errno));
    return false;
  }
  return true;
}

static bool createAudioClient(PipeWireRuntime &runtime) {
  runtime.inputEvents.version = PW_VERSION_STREAM_EVENTS;
  runtime.inputEvents.state_changed = streamStateChanged;
  runtime.inputEvents.param_changed = streamParameterChanged;
  runtime.inputEvents.process = inputProcess;
  runtime.outputEvents.version = PW_VERSION_STREAM_EVENTS;
  runtime.outputEvents.state_changed = streamStateChanged;
  runtime.outputEvents.param_changed = streamParameterChanged;
  runtime.outputEvents.process = outputProcess;

  runtime.context = pw_context_new(
      pw_main_loop_get_loop(runtime.mainLoop), nullptr, 0);
  if (runtime.context == nullptr) {
    failRuntime(runtime, systemError("cannot create PipeWire context", -errno));
    return false;
  }
  runtime.core = pw_context_connect(runtime.context, nullptr, 0);
  if (runtime.core == nullptr) {
    failRuntime(runtime, systemError("cannot connect to PipeWire core", -errno));
    return false;
  }
  const auto error = createAudioStreams(runtime);
  if (!error.empty()) {
    failRuntime(runtime, error);
    return false;
  }
  return true;
}

static void readinessTimedOut(void *data, std::uint64_t) {
  auto &runtime = *static_cast<PipeWireRuntime *>(data);
  failRuntime(runtime,
              "timed out while waiting for PipeWire filter negotiation");
}

static void interrupted(void *data, int) {
  completeRuntime(*static_cast<PipeWireRuntime *>(data));
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
  runtime.interruptSource =
      pw_loop_add_signal(loop, SIGINT, interrupted, &runtime);
  runtime.terminateSource =
      pw_loop_add_signal(loop, SIGTERM, interrupted, &runtime);
  if (runtime.interruptSource == nullptr || runtime.terminateSource == nullptr) {
    failRuntime(runtime,
                systemError("cannot install PipeWire signal handlers", -errno));
    return false;
  }
  return true;
}

static std::string validateOptions(const DspPipeline &pipeline,
                                   const PipeWirePipelineOptions &options) {
  if (options.filterName.empty() ||
      options.filterName.find('\0') != std::string::npos) {
    return "PipeWire filter name must not be empty or contain NUL";
  }
  if (options.filterDescription.empty() ||
      options.filterDescription.find('\0') != std::string::npos) {
    return "PipeWire filter description must not be empty or contain NUL";
  }
  if (options.initialPresetPath.string().find('\0') != std::string::npos ||
      options.controlSocketPath.string().find('\0') != std::string::npos) {
    return "preset and control socket paths must not contain NUL";
  }
  if (!isSelectableSampleRate(options.dspSampleRate)) {
    return "DSP sample rate must be 44100, 48000, 96000, 192000, or 384000 Hz";
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
  if (pipeline.sampleRate() != static_cast<float>(options.dspSampleRate) ||
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
        createControlServer(runtime) && createAudioClient(runtime)) {
      const auto result = pw_main_loop_run(runtime.mainLoop);
      if (result < 0 && runtime.error.empty()) {
        failRuntime(runtime, systemError("PipeWire main loop failed", result));
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
