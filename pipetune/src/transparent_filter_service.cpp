#include "pipetune/pipewire_pipeline.h"

#include "audio_bridge.h"
#include "dsp_backend_runtime.h"
#include "dsp_pipeline_slot.h"
#include "pipewire_buffer_io.h"
#include "pipewire_rate_parser.h"
#include "transparent_filter_output.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/node.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/debug/types.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/audio/raw-utils.h>
#include <spa/param/buffers.h>
#include <spa/param/format.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <time.h>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pipetune {

constexpr auto kFilterSampleBytes = std::uint32_t{sizeof(float)};
constexpr auto kFilterReadinessTimeoutSeconds = std::time_t{10};

struct FilterServiceRuntime;
struct OutputFilterRuntime;

struct FilterStreamContext {
  OutputFilterRuntime *runtime;
  bool capture;
};

struct PhysicalOutputNode {
  FilterServiceRuntime *runtime;
  std::uint32_t id;
  pw_node *node;
  pw_node_events events;
  spa_hook listener;
  TransparentFilterOutputCandidate candidate;
  std::vector<SampleRateConstraint> pendingConstraints;
  PipeWireRateParameterAvailability parameterAvailability;
  bool listenerInstalled;
};

struct OutputFilterRuntime {
  FilterServiceRuntime *service;
  TransparentFilterOutput output;
  std::uint32_t latencyFrames;
  std::unique_ptr<DspPipelineSlot> pipeline;
  PlanarAudioRing ring;
  std::vector<float> captureScratch;
  std::vector<float> playbackScratch;
  std::atomic<std::uint64_t> processingErrors;
  std::uint64_t processedFrames;
  pw_stream *captureStream;
  pw_stream *playbackStream;
  pw_stream_events captureEvents;
  pw_stream_events playbackEvents;
  spa_hook captureListener;
  spa_hook playbackListener;
  FilterStreamContext captureContext;
  FilterStreamContext playbackContext;
  bool captureListenerInstalled;
  bool playbackListenerInstalled;
  bool captureReady;
  bool playbackReady;
  bool captureFormatReady;
  bool playbackFormatReady;
  bool failed;
  bool enabledPublished;
  std::uint32_t mainNodeId;
  std::string error;

  OutputFilterRuntime(FilterServiceRuntime &owner,
                      TransparentFilterOutput runtimeOutput,
                      std::unique_ptr<DspPipeline> preparedPipeline,
                      const PipeWireFilterServiceOptions &options)
      : service(&owner), output(std::move(runtimeOutput)),
        latencyFrames(preparedPipeline == nullptr
                          ? 0
                          : preparedPipeline->latencyFrames()),
        pipeline(preparedPipeline == nullptr
                     ? nullptr
                     : std::make_unique<DspPipelineSlot>(
                           std::move(preparedPipeline))),
        ring(output.channelCount, options.ringCapacityFrames),
        captureScratch(static_cast<std::size_t>(output.channelCount) *
                           options.maxFrames,
                       0.0F),
        playbackScratch(static_cast<std::size_t>(output.channelCount) *
                            options.maxFrames,
                        0.0F),
        processingErrors(0), processedFrames(0), captureStream(nullptr),
        playbackStream(nullptr), captureEvents{}, playbackEvents{},
        captureListener{}, playbackListener{}, captureContext{this, true},
        playbackContext{this, false}, captureListenerInstalled(false),
        playbackListenerInstalled(false), captureReady(false),
        playbackReady(false), captureFormatReady(false),
        playbackFormatReady(false), failed(false), enabledPublished(false),
        mainNodeId(PW_ID_ANY), error() {}
};

struct FilterServiceRuntime {
  std::unique_ptr<DspPipeline> recipe;
  PipeWireFilterServiceOptions options;
  PipeWireRunMode mode;
  DspBackendRuntimeState dspBackendState;
  ProcessingMode processingMode;
  std::string activePreset;
  std::string configurationError;
  TransparentFilterOutputTracker outputTracker;
  pw_main_loop *mainLoop;
  pw_context *context;
  pw_core *core;
  pw_registry *registry;
  pw_metadata *policyMetadata;
  pw_core_events coreEvents;
  pw_registry_events registryEvents;
  pw_metadata_events policyMetadataEvents;
  spa_hook coreListener;
  spa_hook registryListener;
  spa_hook policyMetadataListener;
  spa_source *timeoutSource;
  spa_source *interruptSource;
  spa_source *terminateSource;
  std::unique_ptr<ControlServer> controlServer;
  std::mutex controlStatusMutex;
  std::optional<ControlRuntimeStatus> controlStatusSnapshot;
  std::unordered_map<std::uint32_t, std::unique_ptr<PhysicalOutputNode>>
      physicalNodes;
  std::unordered_map<std::uint32_t, std::unique_ptr<OutputFilterRuntime>>
      filters;
  std::uint64_t retiredOverrunFrames;
  std::uint64_t retiredUnderrunFrames;
  std::uint64_t retiredProcessingErrors;
  int enumerationSequence;
  std::uint32_t policyMetadataId;
  bool initialBindingsSynchronized;
  bool enumerationReady;
  bool reconcilingOutputs;
  bool policyReady;
  bool readyNotified;
  bool completed;
  std::string policyProtocol;
  std::string policyBackend;
  std::string policyState;
  std::string error;

  FilterServiceRuntime(
      std::unique_ptr<DspPipeline> source,
      const PipeWireFilterServiceOptions &runtimeOptions,
      PipeWireRunMode runtimeMode)
      : recipe(std::move(source)), options(runtimeOptions), mode(runtimeMode),
        dspBackendState(makeDspBackendRuntimeState(
            runtimeOptions.dspBackends,
            runtimeOptions.configuredDspBackend,
            runtimeOptions.configuredDspSimdVariant)),
        processingMode(recipe->backendKind().has_value()
                           ? ProcessingMode::preset
                           : ProcessingMode::bypass),
        activePreset(processingMode == ProcessingMode::preset
                         ? runtimeOptions.initialPresetPath.string()
                         : std::string{}),
        configurationError(runtimeOptions.initialConfigurationError),
        outputTracker(runtimeOptions.ratePolicy), mainLoop(nullptr),
        context(nullptr), core(nullptr), registry(nullptr),
        policyMetadata(nullptr), coreEvents{}, registryEvents{},
        policyMetadataEvents{}, coreListener{}, registryListener{},
        policyMetadataListener{}, timeoutSource(nullptr),
        interruptSource(nullptr), terminateSource(nullptr),
        controlServer(nullptr), controlStatusMutex(),
        controlStatusSnapshot(std::nullopt), physicalNodes(), filters(),
        retiredOverrunFrames(0), retiredUnderrunFrames(0),
        retiredProcessingErrors(0), enumerationSequence(0),
        policyMetadataId(PW_ID_ANY), initialBindingsSynchronized(false),
        enumerationReady(false), reconcilingOutputs(false),
        policyReady(false), readyNotified(false), completed(false),
        policyProtocol(), policyBackend(), policyState(), error() {}
};

struct FilterServicePipeWireScope {
  FilterServicePipeWireScope() {
    pw_init(nullptr, nullptr);
  }

  ~FilterServicePipeWireScope() {
    pw_deinit();
  }
};

static std::string filterSystemError(std::string_view operation, int result) {
  const auto errorNumber = result < 0 ? -result : errno;
  return std::string(operation) + ": " + std::strerror(errorNumber);
}

static const char *dictionaryValue(const spa_dict *dictionary,
                                   const char *key) noexcept {
  return dictionary == nullptr ? nullptr
                               : spa_dict_lookup(dictionary, key);
}

static std::string dictionaryString(const spa_dict *dictionary,
                                    const char *key) {
  const auto *value = dictionaryValue(dictionary, key);
  return value == nullptr ? std::string{} : std::string(value);
}

static bool filterBooleanValue(const char *value) noexcept {
  if (value == nullptr) {
    return false;
  }
  return std::string_view(value) == "1" ||
         std::string_view(value) == "true" ||
         std::string_view(value) == "yes" ||
         std::string_view(value) == "on";
}

static std::uint32_t filterUnsignedValue(const char *value) noexcept {
  if (value == nullptr) {
    return 0;
  }
  auto parsed = std::uint32_t{0};
  const auto text = std::string_view(value);
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), parsed);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size()
             ? parsed
             : 0;
}

static std::vector<std::uint32_t>
parseChannelPositions(std::string_view value) {
  auto positions = std::vector<std::uint32_t>{};
  auto token = std::string{};
  const auto appendToken = [&positions](std::string &current) {
    if (current.empty()) {
      return true;
    }
    const auto position =
        spa_debug_type_find_type_short(spa_type_audio_channel,
                                       current.c_str());
    current.clear();
    if (position == SPA_ID_INVALID) {
      return false;
    }
    positions.push_back(position);
    return true;
  };
  for (const auto character : value) {
    if (character == '[' || character == ']' || character == ',' ||
        character == ' ' || character == '\t' || character == '\n') {
      if (!appendToken(token)) {
        return {};
      }
    } else {
      token.push_back(character);
    }
  }
  if (!appendToken(token)) {
    return {};
  }
  return positions;
}

static TransparentFilterOutputCandidate candidateFromProperties(
    std::uint32_t id, const spa_dict *properties) {
  const auto linkGroup = dictionaryValue(properties, PW_KEY_NODE_LINK_GROUP);
  const auto deviceId = dictionaryValue(properties, PW_KEY_DEVICE_ID);
  return {
      .id = id,
      .mediaClass = dictionaryString(properties, PW_KEY_MEDIA_CLASS),
      .nodeName = dictionaryString(properties, PW_KEY_NODE_NAME),
      .description = dictionaryString(properties, PW_KEY_NODE_DESCRIPTION),
      .deviceApi = dictionaryString(properties, "device.api"),
      .hasDevice = deviceId != nullptr &&
                   filterUnsignedValue(deviceId) != PW_ID_ANY,
      .virtualNode =
          filterBooleanValue(dictionaryValue(properties, PW_KEY_NODE_VIRTUAL)),
      .networkNode =
          filterBooleanValue(dictionaryValue(properties, "node.network")),
      .encodedOnly =
          filterBooleanValue(
              dictionaryValue(properties, "item.node.encoded-only")) ||
          filterBooleanValue(
              dictionaryValue(properties, "node.encoded-only")),
      .smartFilter =
          filterBooleanValue(dictionaryValue(properties, "filter.smart")) ||
          filterBooleanValue(dictionaryValue(properties, "pipetune.filter")) ||
          (linkGroup != nullptr && linkGroup[0] != '\0'),
      .channelCount = filterUnsignedValue(
          dictionaryValue(properties, SPA_KEY_AUDIO_CHANNELS)),
      .channelPositions = parseChannelPositions(
          dictionaryString(properties, SPA_KEY_AUDIO_POSITION)),
      .sampleRateCapabilities = {},
      .activeSampleRate = filterUnsignedValue(
          dictionaryValue(properties, SPA_KEY_AUDIO_RATE)),
  };
}

static TransparentFilterOutputCandidate mergeCandidateProperties(
    const TransparentFilterOutputCandidate &current,
    const spa_dict *properties) {
  auto updated = candidateFromProperties(current.id, properties);
  if (dictionaryValue(properties, PW_KEY_MEDIA_CLASS) == nullptr) {
    updated.mediaClass = current.mediaClass;
  }
  if (dictionaryValue(properties, PW_KEY_NODE_NAME) == nullptr) {
    updated.nodeName = current.nodeName;
  }
  if (dictionaryValue(properties, PW_KEY_NODE_DESCRIPTION) == nullptr) {
    updated.description = current.description;
  }
  if (dictionaryValue(properties, "device.api") == nullptr) {
    updated.deviceApi = current.deviceApi;
  }
  if (dictionaryValue(properties, PW_KEY_DEVICE_ID) == nullptr) {
    updated.hasDevice = current.hasDevice;
  }
  if (dictionaryValue(properties, PW_KEY_NODE_VIRTUAL) == nullptr) {
    updated.virtualNode = current.virtualNode;
  }
  if (dictionaryValue(properties, "node.network") == nullptr) {
    updated.networkNode = current.networkNode;
  }
  if (dictionaryValue(properties, "item.node.encoded-only") == nullptr &&
      dictionaryValue(properties, "node.encoded-only") == nullptr) {
    updated.encodedOnly = current.encodedOnly;
  }
  if (dictionaryValue(properties, "filter.smart") == nullptr &&
      dictionaryValue(properties, "pipetune.filter") == nullptr &&
      dictionaryValue(properties, PW_KEY_NODE_LINK_GROUP) == nullptr) {
    updated.smartFilter = current.smartFilter;
  }
  if (dictionaryValue(properties, SPA_KEY_AUDIO_CHANNELS) == nullptr) {
    updated.channelCount = current.channelCount;
  }
  if (dictionaryValue(properties, SPA_KEY_AUDIO_POSITION) == nullptr) {
    updated.channelPositions = current.channelPositions;
  }
  updated.sampleRateCapabilities = current.sampleRateCapabilities;
  updated.activeSampleRate = current.activeSampleRate;
  return updated;
}

static void failFilterService(FilterServiceRuntime &runtime,
                              std::string error) {
  if (!runtime.error.empty()) {
    return;
  }
  runtime.error = std::move(error);
  if (runtime.mainLoop != nullptr) {
    pw_main_loop_quit(runtime.mainLoop);
  }
}

static bool outputFilterSettled(const OutputFilterRuntime &runtime) noexcept {
  return runtime.failed ||
         (runtime.captureReady && runtime.playbackReady &&
          runtime.captureFormatReady && runtime.playbackFormatReady);
}

static bool outputFilterReady(const OutputFilterRuntime &runtime) noexcept {
  return !runtime.failed && runtime.captureReady && runtime.playbackReady &&
         runtime.captureFormatReady && runtime.playbackFormatReady;
}

static void publishFilterEnabled(OutputFilterRuntime &runtime);
static void maybeCompleteFilterServiceReadiness(FilterServiceRuntime &runtime);
static void reconcileOutputFilters(FilterServiceRuntime &runtime);
static void refreshControlStatusSnapshot(FilterServiceRuntime &runtime);

static void refreshPolicyReady(FilterServiceRuntime &runtime) {
  const auto ready = runtime.policyMetadata != nullptr &&
                     runtime.policyProtocol == "1" &&
                     runtime.policyState == "ready" &&
                     (runtime.policyBackend == "wireplumber-0.4" ||
                      runtime.policyBackend == "wireplumber-0.5");
  if (ready == runtime.policyReady) {
    return;
  }
  runtime.policyReady = ready;
  for (auto &[id, filter] : runtime.filters) {
    static_cast<void>(id);
    publishFilterEnabled(*filter);
  }
  refreshControlStatusSnapshot(runtime);
}

static int policyMetadataProperty(void *data, std::uint32_t subject,
                                  const char *key, const char *,
                                  const char *value) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  if (subject != 0 || key == nullptr) {
    return 0;
  }
  auto *destination = static_cast<std::string *>(nullptr);
  if (std::string_view(key) == "protocol.version") {
    destination = &runtime.policyProtocol;
  } else if (std::string_view(key) == "policy.backend") {
    destination = &runtime.policyBackend;
  } else if (std::string_view(key) == "policy.state") {
    destination = &runtime.policyState;
  }
  if (destination != nullptr) {
    *destination = value == nullptr ? std::string{} : std::string(value);
    refreshPolicyReady(runtime);
  }
  return 0;
}

static std::string jsonQuoted(std::string_view value) {
  auto result = std::string{"\""};
  for (const auto character : value) {
    if (character == '\\' || character == '"') {
      result.push_back('\\');
    }
    result.push_back(character);
  }
  result.push_back('"');
  return result;
}

static std::string channelPositionString(
    std::span<const std::uint32_t> positions) {
  auto result = std::string{};
  for (const auto position : positions) {
    const auto *name =
        spa_debug_type_find_short_name(spa_type_audio_channel, position);
    if (name == nullptr) {
      return {};
    }
    if (!result.empty()) {
      result.push_back(',');
    }
    result.append(name);
  }
  return result;
}

static pw_properties *makeFilterCommonProperties(
    const OutputFilterRuntime &runtime, std::string_view nodeName,
    std::uint32_t nodeRate) {
  auto *properties = pw_properties_new(nullptr, nullptr);
  if (properties == nullptr) {
    return nullptr;
  }
  const auto mediaRate = std::to_string(runtime.output.rates.dspSampleRate);
  const auto channels = std::to_string(runtime.output.channelCount);
  const auto graphRate = "1/" + std::to_string(nodeRate);
  const auto positions = channelPositionString(runtime.output.channelPositions);
  const auto nodeGroup = runtime.output.filterNodeName + ".group";
  pw_properties_set(properties, PW_KEY_APP_NAME, "PipeTune");
  pw_properties_set(properties, PW_KEY_MEDIA_TYPE, "Audio");
  pw_properties_set(properties, PW_KEY_MEDIA_ROLE, "DSP");
  pw_properties_set(properties, PW_KEY_NODE_NAME,
                    std::string(nodeName).c_str());
  pw_properties_set(properties, PW_KEY_NODE_GROUP, nodeGroup.c_str());
  pw_properties_set(properties, PW_KEY_NODE_LINK_GROUP,
                    runtime.output.filterLinkGroup.c_str());
  pw_properties_set(properties, PW_KEY_NODE_RATE, graphRate.c_str());
  pw_properties_set(properties, SPA_KEY_AUDIO_FORMAT, "F32P");
  pw_properties_set(properties, SPA_KEY_AUDIO_RATE, mediaRate.c_str());
  pw_properties_set(properties, SPA_KEY_AUDIO_CHANNELS, channels.c_str());
  pw_properties_set(properties, SPA_KEY_AUDIO_POSITION, positions.c_str());
  pw_properties_set(properties, "state.restore-props", "false");
  pw_properties_set(properties, "stream.dont-remix", "true");
  pw_properties_set(properties, "channelmix.min-volume", "1.0");
  pw_properties_set(properties, "channelmix.max-volume", "1.0");
  pw_properties_set(properties, "pipetune.target.node",
                    runtime.output.nodeName.c_str());
  return properties;
}

static pw_properties *makeFilterCaptureProperties(
    const OutputFilterRuntime &runtime) {
  auto *properties = makeFilterCommonProperties(
      runtime, runtime.output.filterNodeName,
      runtime.output.rates.dspSampleRate);
  if (properties == nullptr) {
    return nullptr;
  }
  const auto description =
      "PipeTune for " +
      (runtime.output.description.empty() ? runtime.output.nodeName
                                          : runtime.output.description);
  const auto targetJson =
      "{ \"node.name\": " + jsonQuoted(runtime.output.nodeName) + " }";
  pw_properties_set(properties, PW_KEY_MEDIA_CLASS, "Audio/Sink");
  pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
  pw_properties_set(properties, PW_KEY_NODE_DESCRIPTION,
                    description.c_str());
  pw_properties_set(properties, PW_KEY_NODE_VIRTUAL, "true");
  pw_properties_set(properties, PW_KEY_NODE_ALWAYS_PROCESS, "true");
  pw_properties_set(properties, "filter.smart", "true");
  pw_properties_set(properties, "filter.smart.name",
                    runtime.output.filterNodeName.c_str());
  pw_properties_set(properties, "filter.smart.target",
                    targetJson.c_str());
  pw_properties_set(properties, "filter.smart.targetable", "false");
  // Policy must explicitly enable a fully negotiated filter. This static
  // value preserves direct routing if WirePlumber or its helper is absent.
  pw_properties_set(properties, "filter.smart.disabled", "true");
  pw_properties_set(properties, "pipetune.filter", "true");
  return properties;
}

static pw_properties *makeFilterPlaybackProperties(
    const OutputFilterRuntime &runtime) {
  auto *properties = makeFilterCommonProperties(
      runtime, runtime.output.filterNodeName + ".output",
      runtime.output.rates.outputSampleRate);
  if (properties == nullptr) {
    return nullptr;
  }
  pw_properties_set(properties, PW_KEY_MEDIA_CLASS, "Stream/Output/Audio");
  pw_properties_set(properties, PW_KEY_MEDIA_CATEGORY, "Playback");
  pw_properties_set(properties, PW_KEY_NODE_PASSIVE, "true");
  pw_properties_set(properties, PW_KEY_TARGET_OBJECT,
                    runtime.output.nodeName.c_str());
  pw_properties_set(properties, "node.dont-reconnect", "true");
  pw_properties_set(properties, "pipetune.filter.stream", "true");
  if (runtime.service->options.ratePolicy.enforcement ==
      SampleRateEnforcement::force) {
    pw_properties_set(properties, PW_KEY_NODE_FORCE_RATE, "0");
  }
  return properties;
}

static spa_audio_info_raw makeFilterRawFormat(
    const OutputFilterRuntime &runtime) {
  auto format = spa_audio_info_raw{};
  format.format = SPA_AUDIO_FORMAT_F32P;
  format.rate = runtime.output.rates.dspSampleRate;
  format.channels = runtime.output.channelCount;
  for (auto channel = std::uint32_t{0};
       channel < runtime.output.channelCount; ++channel) {
    format.position[channel] = runtime.output.channelPositions[channel];
  }
  return format;
}

static spa_pod *buildFilterBufferParameter(
    spa_pod_builder &builder, const OutputFilterRuntime &runtime) {
  auto frame = spa_pod_frame{};
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_ParamBuffers,
                              SPA_PARAM_Buffers);
  spa_pod_builder_add(
      &builder, SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 32),
      SPA_PARAM_BUFFERS_blocks,
      SPA_POD_Int(static_cast<int>(runtime.output.channelCount)),
      SPA_PARAM_BUFFERS_size,
      SPA_POD_CHOICE_RANGE_Int(
          static_cast<int>(runtime.service->options.maxFrames *
                           kFilterSampleBytes),
          static_cast<int>(32 * kFilterSampleBytes),
          static_cast<int>(runtime.service->options.maxFrames *
                           kFilterSampleBytes)),
      SPA_PARAM_BUFFERS_stride,
      SPA_POD_Int(static_cast<int>(kFilterSampleBytes)),
      SPA_PARAM_BUFFERS_dataType,
      SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) |
                               (1 << SPA_DATA_MemFd) |
                               (1 << SPA_DATA_MemId)),
      0);
  return static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame));
}

static bool filterFormatMatches(const OutputFilterRuntime &runtime,
                                const spa_audio_info_raw &format) noexcept {
  if (format.format != SPA_AUDIO_FORMAT_F32P ||
      format.rate != runtime.output.rates.dspSampleRate ||
      format.channels != runtime.output.channelCount) {
    return false;
  }
  for (auto channel = std::uint32_t{0};
       channel < runtime.output.channelCount; ++channel) {
    if (format.position[channel] != runtime.output.channelPositions[channel]) {
      return false;
    }
  }
  return true;
}

static void disableFilterAlwaysProcess(OutputFilterRuntime &runtime) {
  if (runtime.captureStream == nullptr) {
    return;
  }
  const auto item = spa_dict_item{PW_KEY_NODE_ALWAYS_PROCESS, "false"};
  const auto dictionary = spa_dict{0, 1, &item};
  static_cast<void>(
      pw_stream_update_properties(runtime.captureStream, &dictionary));
}

static void markOutputFilterFailed(OutputFilterRuntime &runtime,
                                   std::string error) {
  if (runtime.failed) {
    return;
  }
  runtime.failed = true;
  runtime.error = std::move(error);
  publishFilterEnabled(runtime);
  maybeCompleteFilterServiceReadiness(*runtime.service);
}

static void filterStreamStateChanged(void *data, pw_stream_state,
                                     pw_stream_state state,
                                     const char *error) {
  auto &context = *static_cast<FilterStreamContext *>(data);
  auto &runtime = *context.runtime;
  if (state == PW_STREAM_STATE_ERROR) {
    const auto detail =
        error == nullptr ? std::string("unknown PipeWire stream error")
                         : std::string(error);
    markOutputFilterFailed(
        runtime,
        (context.capture ? "filter input: " : "filter output: ") + detail);
    return;
  }
  const auto ready =
      state == PW_STREAM_STATE_PAUSED || state == PW_STREAM_STATE_STREAMING;
  if (context.capture) {
    runtime.captureReady = ready;
  } else {
    runtime.playbackReady = ready;
  }
  if (outputFilterReady(runtime)) {
    disableFilterAlwaysProcess(runtime);
  }
  publishFilterEnabled(runtime);
  maybeCompleteFilterServiceReadiness(*runtime.service);
}

static void filterStreamParameterChanged(void *data, std::uint32_t id,
                                         const spa_pod *parameter) {
  if (id != SPA_PARAM_Format || parameter == nullptr) {
    return;
  }
  auto &context = *static_cast<FilterStreamContext *>(data);
  auto &runtime = *context.runtime;
  auto format = spa_audio_info_raw{};
  if (spa_format_audio_raw_parse(parameter, &format) < 0 ||
      !filterFormatMatches(runtime, format)) {
    markOutputFilterFailed(
        runtime, "PipeWire negotiated a filter format that does not match "
                 "the physical output layout");
    return;
  }

  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  const spa_pod *parameters[] = {
      buildFilterBufferParameter(builder, runtime)};
  auto *stream =
      context.capture ? runtime.captureStream : runtime.playbackStream;
  const auto updateResult =
      pw_stream_update_params(stream, parameters, 1);
  if (updateResult < 0) {
    markOutputFilterFailed(
        runtime,
        filterSystemError("cannot configure PipeWire filter buffers",
                          updateResult));
    return;
  }
  if (context.capture) {
    runtime.captureFormatReady = true;
  } else {
    runtime.playbackFormatReady = true;
  }
  if (outputFilterReady(runtime)) {
    disableFilterAlwaysProcess(runtime);
  }
  publishFilterEnabled(runtime);
  maybeCompleteFilterServiceReadiness(*runtime.service);
}

static void copyFilterCapturePlane(const spa_data &plane,
                                   std::uint32_t sourceFrame,
                                   std::span<float> destination) noexcept {
  if ((plane.chunk->flags & SPA_CHUNK_FLAG_EMPTY) != 0) {
    std::fill(destination.begin(), destination.end(), 0.0F);
    return;
  }
  const auto byteCount =
      static_cast<std::uint32_t>(destination.size_bytes());
  const auto sourceByte =
      (plane.chunk->offset + sourceFrame * kFilterSampleBytes) % plane.maxsize;
  const auto firstBytes = std::min(byteCount, plane.maxsize - sourceByte);
  const auto *source = static_cast<const std::uint8_t *>(plane.data);
  std::memcpy(destination.data(), source + sourceByte, firstBytes);
  std::memcpy(reinterpret_cast<std::uint8_t *>(destination.data()) + firstBytes,
              source, byteCount - firstBytes);
}

static void filterCaptureProcess(void *data) {
  auto &runtime =
      *static_cast<FilterStreamContext *>(data)->runtime;
  auto *pipeWireBuffer = pw_stream_dequeue_buffer(runtime.captureStream);
  if (pipeWireBuffer == nullptr || pipeWireBuffer->buffer == nullptr) {
    return;
  }
  auto &buffer = *pipeWireBuffer->buffer;
  auto frameCount = std::uint32_t{0};
  if (!inspectPipeWireCaptureBuffer(buffer, runtime.output.channelCount,
                                    frameCount)) {
    runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    pipeWireBuffer->size = 0;
    retirePipeWireCaptureBuffer(buffer);
    pw_stream_queue_buffer(runtime.captureStream, pipeWireBuffer);
    return;
  }

  auto sourceFrame = std::uint32_t{0};
  while (sourceFrame < frameCount) {
    const auto blockFrames =
        std::min(runtime.service->options.maxFrames,
                 frameCount - sourceFrame);
    auto scratch = std::span<float>(runtime.captureScratch)
                       .first(static_cast<std::size_t>(
                                  runtime.output.channelCount) *
                              blockFrames);
    for (auto channel = std::uint32_t{0};
         channel < runtime.output.channelCount; ++channel) {
      copyFilterCapturePlane(
          buffer.datas[channel], sourceFrame,
          scratch.subspan(static_cast<std::size_t>(channel) * blockFrames,
                          blockFrames));
    }
    const auto timeSeconds =
        static_cast<double>(runtime.processedFrames) /
        runtime.output.rates.dspSampleRate;
    if (runtime.pipeline == nullptr ||
        runtime.pipeline->process(scratch, runtime.output.channelCount,
                                  blockFrames, timeSeconds) !=
            ProcessStatus::ok) {
      runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    }
    runtime.ring.write(scratch, blockFrames);
    runtime.processedFrames += blockFrames;
    sourceFrame += blockFrames;
  }
  pipeWireBuffer->size = frameCount;
  retirePipeWireCaptureBuffer(buffer);
  pw_stream_queue_buffer(runtime.captureStream, pipeWireBuffer);
}

static bool inspectFilterPlaybackBuffer(
    const spa_buffer &buffer, std::uint32_t channelCount,
    std::uint32_t &capacityFrames) noexcept {
  if (buffer.n_datas < channelCount || buffer.datas == nullptr) {
    return false;
  }
  capacityFrames = UINT32_MAX;
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto &plane = buffer.datas[channel];
    if (plane.data == nullptr || plane.chunk == nullptr ||
        plane.maxsize < kFilterSampleBytes) {
      return false;
    }
    capacityFrames =
        std::min(capacityFrames, plane.maxsize / kFilterSampleBytes);
  }
  return capacityFrames != UINT32_MAX;
}

static void clearFilterPlaybackChunks(spa_buffer &buffer,
                                      std::uint32_t channelCount) noexcept {
  const auto availableChannels = std::min(buffer.n_datas, channelCount);
  for (auto channel = std::uint32_t{0}; channel < availableChannels;
       ++channel) {
    auto &plane = buffer.datas[channel];
    if (plane.chunk != nullptr) {
      plane.chunk->offset = 0;
      plane.chunk->size = 0;
      plane.chunk->stride = static_cast<std::int32_t>(kFilterSampleBytes);
      plane.chunk->flags = SPA_CHUNK_FLAG_EMPTY;
    }
  }
}

static void filterPlaybackProcess(void *data) {
  auto &runtime =
      *static_cast<FilterStreamContext *>(data)->runtime;
  auto *pipeWireBuffer = pw_stream_dequeue_buffer(runtime.playbackStream);
  if (pipeWireBuffer == nullptr || pipeWireBuffer->buffer == nullptr) {
    return;
  }
  auto &buffer = *pipeWireBuffer->buffer;
  auto capacityFrames = std::uint32_t{0};
  if (!inspectFilterPlaybackBuffer(buffer, runtime.output.channelCount,
                                   capacityFrames)) {
    runtime.processingErrors.fetch_add(1, std::memory_order_relaxed);
    clearFilterPlaybackChunks(buffer, runtime.output.channelCount);
    pipeWireBuffer->size = 0;
    pw_stream_queue_buffer(runtime.playbackStream, pipeWireBuffer);
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
        std::min(runtime.service->options.maxFrames,
                 suggestedFrames - outputFrame);
    auto scratch = std::span<float>(runtime.playbackScratch)
                       .first(static_cast<std::size_t>(
                                  runtime.output.channelCount) *
                              blockFrames);
    runtime.ring.read(scratch, blockFrames);
    for (auto channel = std::uint32_t{0};
         channel < runtime.output.channelCount; ++channel) {
      const auto source = scratch.subspan(
          static_cast<std::size_t>(channel) * blockFrames, blockFrames);
      auto *destination = static_cast<float *>(buffer.datas[channel].data);
      std::copy(source.begin(), source.end(), destination + outputFrame);
    }
    outputFrame += blockFrames;
  }
  for (auto channel = std::uint32_t{0};
       channel < runtime.output.channelCount; ++channel) {
    auto &chunk = *buffer.datas[channel].chunk;
    chunk.offset = 0;
    chunk.size = suggestedFrames * kFilterSampleBytes;
    chunk.stride = static_cast<std::int32_t>(kFilterSampleBytes);
    chunk.flags = suggestedFrames == 0 ? SPA_CHUNK_FLAG_EMPTY
                                       : SPA_CHUNK_FLAG_NONE;
  }
  pipeWireBuffer->size = suggestedFrames;
  pw_stream_queue_buffer(runtime.playbackStream, pipeWireBuffer);
}

static std::string connectFilterStream(OutputFilterRuntime &runtime,
                                       pw_stream *stream,
                                       pw_direction direction,
                                       bool autoconnect,
                                       bool publishLatency) {
  auto storage = std::array<std::uint8_t, 2048>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  auto format = makeFilterRawFormat(runtime);
  auto latency = spa_process_latency_info{
      .quantum = 0.0F,
      .rate = runtime.latencyFrames,
      .ns = 0};
  const spa_pod *parameters[2] = {
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format),
      nullptr};
  auto parameterCount = std::uint32_t{1};
  if (publishLatency) {
    parameters[parameterCount++] = spa_process_latency_build(
        &builder, SPA_PARAM_ProcessLatency, &latency);
  }
  auto flags = PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS;
  if (autoconnect) {
    flags |= PW_STREAM_FLAG_AUTOCONNECT;
    flags |= PW_STREAM_FLAG_DONT_RECONNECT;
  }
  const auto result = pw_stream_connect(
      stream, direction, PW_ID_ANY, static_cast<pw_stream_flags>(flags),
      parameters, parameterCount);
  return result < 0
             ? filterSystemError("cannot connect PipeWire filter stream",
                                 result)
             : std::string{};
}

static void publishFilterEnabled(OutputFilterRuntime &runtime) {
  auto &service = *runtime.service;
  if (service.policyMetadata == nullptr || runtime.captureStream == nullptr) {
    runtime.enabledPublished = false;
    return;
  }
  const auto nodeId = pw_stream_get_node_id(runtime.captureStream);
  if (nodeId == PW_ID_ANY) {
    return;
  }
  runtime.mainNodeId = nodeId;
  const auto enabled = service.policyReady && outputFilterReady(runtime);
  const auto result = pw_metadata_set_property(
      service.policyMetadata, nodeId, "filter.enabled", "Spa:String",
      enabled ? "true" : "false");
  runtime.enabledPublished = result >= 0 && enabled;
  if (result < 0 && enabled) {
    markOutputFilterFailed(
        runtime,
        filterSystemError("cannot enable WirePlumber filter policy", result));
  }
}

static void destroyFilterStreams(OutputFilterRuntime &runtime) {
  if (runtime.service->policyMetadata != nullptr &&
      runtime.mainNodeId != PW_ID_ANY) {
    static_cast<void>(pw_metadata_set_property(
        runtime.service->policyMetadata, runtime.mainNodeId,
        "filter.enabled", "Spa:String", "false"));
  }
  if (runtime.playbackStream != nullptr) {
    if (runtime.playbackListenerInstalled) {
      spa_hook_remove(&runtime.playbackListener);
      runtime.playbackListenerInstalled = false;
    }
    pw_stream_destroy(runtime.playbackStream);
    runtime.playbackStream = nullptr;
  }
  if (runtime.captureStream != nullptr) {
    if (runtime.captureListenerInstalled) {
      spa_hook_remove(&runtime.captureListener);
      runtime.captureListenerInstalled = false;
    }
    pw_stream_destroy(runtime.captureStream);
    runtime.captureStream = nullptr;
  }
}

static std::string createFilterStreams(OutputFilterRuntime &runtime) {
  runtime.captureEvents.version = PW_VERSION_STREAM_EVENTS;
  runtime.captureEvents.state_changed = filterStreamStateChanged;
  runtime.captureEvents.param_changed = filterStreamParameterChanged;
  runtime.captureEvents.process = filterCaptureProcess;
  runtime.playbackEvents.version = PW_VERSION_STREAM_EVENTS;
  runtime.playbackEvents.state_changed = filterStreamStateChanged;
  runtime.playbackEvents.param_changed = filterStreamParameterChanged;
  runtime.playbackEvents.process = filterPlaybackProcess;

  auto *captureProperties = makeFilterCaptureProperties(runtime);
  if (captureProperties == nullptr) {
    return "cannot allocate PipeWire filter input properties";
  }
  runtime.captureStream = pw_stream_new(runtime.service->core,
                                        "PipeTune filter input",
                                        captureProperties);
  if (runtime.captureStream == nullptr) {
    return filterSystemError("cannot create PipeWire filter input", -errno);
  }
  pw_stream_add_listener(runtime.captureStream, &runtime.captureListener,
                         &runtime.captureEvents, &runtime.captureContext);
  runtime.captureListenerInstalled = true;
  auto connectionError = connectFilterStream(
      runtime, runtime.captureStream, PW_DIRECTION_INPUT, false, true);
  if (!connectionError.empty()) {
    return connectionError;
  }

  auto *playbackProperties = makeFilterPlaybackProperties(runtime);
  if (playbackProperties == nullptr) {
    return "cannot allocate PipeWire filter output properties";
  }
  runtime.playbackStream = pw_stream_new(runtime.service->core,
                                         "PipeTune filter output",
                                         playbackProperties);
  if (runtime.playbackStream == nullptr) {
    return filterSystemError("cannot create PipeWire filter output", -errno);
  }
  pw_stream_add_listener(runtime.playbackStream, &runtime.playbackListener,
                         &runtime.playbackEvents, &runtime.playbackContext);
  runtime.playbackListenerInstalled = true;
  connectionError = connectFilterStream(
      runtime, runtime.playbackStream, PW_DIRECTION_OUTPUT, true, false);
  return connectionError;
}

static void retireOutputFilter(FilterServiceRuntime &service,
                               OutputFilterRuntime &runtime) {
  service.retiredOverrunFrames += runtime.ring.overrunFrames();
  service.retiredUnderrunFrames += runtime.ring.underrunFrames();
  service.retiredProcessingErrors +=
      runtime.processingErrors.load(std::memory_order_relaxed);
  destroyFilterStreams(runtime);
}

static std::unique_ptr<OutputFilterRuntime> createOutputFilter(
    FilterServiceRuntime &service, const TransparentFilterOutput &output) {
  auto rebuilt = rebuildDspPipeline(
      *service.recipe,
      {.sampleRate = static_cast<float>(output.rates.dspSampleRate),
       .maxChannels = output.channelCount,
       .maxFrames = service.options.maxFrames});
  auto runtime = std::make_unique<OutputFilterRuntime>(
      service, output, std::move(rebuilt.pipeline), service.options);
  if (runtime->pipeline == nullptr) {
    runtime->failed = true;
    runtime->error = "cannot prepare output DSP: " + rebuilt.error;
    return runtime;
  }
  const auto streamError = createFilterStreams(*runtime);
  if (!streamError.empty()) {
    runtime->failed = true;
    runtime->error = streamError;
    destroyFilterStreams(*runtime);
  }
  return runtime;
}

struct OutputFilterStateSnapshot {
  PipeWireFilterOutputState state;
  std::string error;
  std::uint32_t latencyFrames;
};

static OutputFilterStateSnapshot outputFilterState(
    const FilterServiceRuntime &runtime,
    const TransparentFilterOutput &output) {
  const auto found = runtime.filters.find(output.id);
  if (found == runtime.filters.end()) {
    return {.state = PipeWireFilterOutputState::bypassed,
            .error = "output filter is not running",
            .latencyFrames = 0};
  }
  const auto &filter = *found->second;
  if (filter.failed) {
    return {.state = PipeWireFilterOutputState::error,
            .error = filter.error,
            .latencyFrames = filter.latencyFrames};
  }
  if (!outputFilterReady(filter)) {
    return {.state = PipeWireFilterOutputState::waiting,
            .error = {},
            .latencyFrames = filter.latencyFrames};
  }
  if (!runtime.policyReady || !filter.enabledPublished) {
    return {
        .state = PipeWireFilterOutputState::bypassed,
        .error = "WirePlumber PipeTune policy handshake is unavailable",
        .latencyFrames = filter.latencyFrames};
  }
  return {.state = PipeWireFilterOutputState::active,
          .error = {},
          .latencyFrames = filter.latencyFrames};
}

static PipeWireFilterOutputState rejectedOutputState(
    const TransparentFilterRejectedOutput &output) noexcept {
  return output.rejection ==
                     TransparentFilterOutputRejection::unsupportedLayout ||
                 output.rejection ==
                     TransparentFilterOutputRejection::invalidRatePolicy
             ? PipeWireFilterOutputState::error
             : PipeWireFilterOutputState::bypassed;
}

static ControlFilterState controlFilterState(
    PipeWireFilterOutputState state) noexcept {
  switch (state) {
  case PipeWireFilterOutputState::waiting:
    return ControlFilterState::waiting;
  case PipeWireFilterOutputState::active:
    return ControlFilterState::active;
  case PipeWireFilterOutputState::bypassed:
    return ControlFilterState::bypassed;
  case PipeWireFilterOutputState::error:
    return ControlFilterState::error;
  }
  return ControlFilterState::error;
}

static ControlDspBackendAvailability filterControlBackendAvailability(
    DspBackendKind kind, const DspBackendLoadResult &result) {
  auto cpuRequirement = result.cpuRequirement;
  if (cpuRequirement.empty()) {
    cpuRequirement =
        kind == DspBackendKind::scalar ? "none" : "unknown";
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

static ControlDspVariantAvailability filterControlVariantAvailability(
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

static ControlRuntimeStatus buildFilterControlStatus(
    const FilterServiceRuntime &runtime) {
  auto filterOutputs = std::vector<ControlFilterOutputStatus>{};
  filterOutputs.reserve(runtime.outputTracker.outputs().size() +
                        runtime.outputTracker.rejectedOutputs().size());
  auto overrunFrames = runtime.retiredOverrunFrames;
  auto underrunFrames = runtime.retiredUnderrunFrames;
  auto processingErrors = runtime.retiredProcessingErrors;
  auto dspProcessedFrames = std::uint64_t{0};
  auto dspProcessingNanoseconds = std::uint64_t{0};

  for (const auto &output : runtime.outputTracker.outputs()) {
    const auto status = outputFilterState(runtime, output);
    const auto found = runtime.filters.find(output.id);
    auto outputOverruns = std::uint64_t{0};
    auto outputUnderruns = std::uint64_t{0};
    auto outputProcessingErrors = std::uint64_t{0};
    auto performance = DspPerformanceCounters{};
    if (found != runtime.filters.end()) {
      outputOverruns = found->second->ring.overrunFrames();
      outputUnderruns = found->second->ring.underrunFrames();
      outputProcessingErrors =
          found->second->processingErrors.load(std::memory_order_relaxed);
      if (found->second->pipeline != nullptr) {
        performance = found->second->pipeline->performanceCounters();
      }
      overrunFrames += outputOverruns;
      underrunFrames += outputUnderruns;
      processingErrors += outputProcessingErrors;
      dspProcessedFrames += performance.processedFrames;
      dspProcessingNanoseconds += performance.processingNanoseconds;
    }
    filterOutputs.push_back(
        {.targetNodeName = output.nodeName,
         .targetDescription = output.description,
         .filterNodeName = output.filterNodeName,
         .state = controlFilterState(status.state),
         .error = status.error,
         .channelCount = output.channelCount,
         .sampleRateCapabilities = output.sampleRateCapabilities,
         .dspSampleRate = output.rates.dspSampleRate,
         .outputSampleRate = output.rates.outputSampleRate,
         .activeOutputSampleRate = output.activeSampleRate,
         .rateFallback = output.rates.fallback,
         .latencyFrames = status.latencyFrames,
         .overrunFrames = outputOverruns,
         .underrunFrames = outputUnderruns,
         .processingErrors = outputProcessingErrors,
         .dspProcessedFrames = performance.processedFrames,
         .dspProcessingNanoseconds =
             performance.processingNanoseconds});
  }
  for (const auto &output : runtime.outputTracker.rejectedOutputs()) {
    if (output.nodeName.empty()) {
      continue;
    }
    const auto state = rejectedOutputState(output);
    filterOutputs.push_back(
        {.targetNodeName = output.nodeName,
         .targetDescription = output.description,
         .filterNodeName = {},
         .state = controlFilterState(state),
         .error = output.error,
         .channelCount = 0,
         .sampleRateCapabilities = output.sampleRateCapabilities,
         .dspSampleRate = 0,
         .outputSampleRate = 0,
         .activeOutputSampleRate = 0,
         .rateFallback = false,
         .latencyFrames = 0,
         .overrunFrames = 0,
         .underrunFrames = 0,
         .processingErrors = 0,
         .dspProcessedFrames = 0,
         .dspProcessingNanoseconds = 0});
  }

  auto availableBackends =
      std::array<ControlDspBackendAvailability, 2>{
          filterControlBackendAvailability(
              DspBackendKind::scalar,
              runtime.dspBackendState.backends.scalar),
          filterControlBackendAvailability(
              DspBackendKind::simd,
              runtime.dspBackendState.backends.simd)};
  auto availableVariants =
      std::vector<ControlDspVariantAvailability>{};
  availableVariants.reserve(
      runtime.dspBackendState.backends.simdVariants.size() + 1u);
  availableVariants.push_back(filterControlVariantAvailability(
      runtime.dspBackendState.backends.scalar));
  for (const auto &variant :
       runtime.dspBackendState.backends.simdVariants) {
    availableVariants.push_back(
        filterControlVariantAvailability(variant));
  }

  return {
      .processingMode = runtime.processingMode,
      .activePreset = runtime.activePreset,
      .configurationError = runtime.configurationError,
      .activePluginCount = runtime.recipe->activePluginCount(),
      .policyBackend = runtime.policyBackend,
      .filterOutputs = std::move(filterOutputs),
      .preferredTarget = {},
      .selectedTarget = {},
      .outputSelectionReason = ControlOutputSelectionReason::unavailable,
      .availableOutputs = {},
      .defaultSinkActive = false,
      .overrunFrames = overrunFrames,
      .underrunFrames = underrunFrames,
      .processingErrors = processingErrors,
      .dspProcessedFrames = dspProcessedFrames,
      .dspProcessingNanoseconds = dspProcessingNanoseconds,
      .inputSampleFormat = {},
      .inputSampleRate = 0,
      .inputChannelCount = 0,
      .inputFramesReceived = 0,
      .inputLastReceivedUnixMilliseconds = 0,
      .configuredRatePolicy = runtime.options.ratePolicy,
      .dspSampleRate = 0,
      .selectedOutputSampleRate = 0,
      .activeOutputSampleRate = 0,
      .rateTransitioning = false,
      .rateFallback = false,
      .rateError = {},
      .configuredDspBackend = runtime.dspBackendState.configuredBackend,
      .configuredDspSimdVariant =
          runtime.dspBackendState.configuredSimdVariant,
      .effectiveDspBackend = runtime.dspBackendState.effectiveBackend,
      .effectiveDspVariant = runtime.dspBackendState.effectiveVariant,
      .dspBackendFallback = runtime.dspBackendState.fallback,
      .dspBackendError = runtime.dspBackendState.error,
      .availableDspBackends = std::move(availableBackends),
      .availableDspVariants = std::move(availableVariants)};
}

static void refreshControlStatusSnapshot(FilterServiceRuntime &runtime) {
  auto status = buildFilterControlStatus(runtime);
  {
    auto lock = std::scoped_lock(runtime.controlStatusMutex);
    runtime.controlStatusSnapshot = std::move(status);
  }
  publishControlStatus(runtime.controlServer.get());
}

static void maybeCompleteFilterServiceReadiness(FilterServiceRuntime &runtime) {
  refreshControlStatusSnapshot(runtime);
  if (!runtime.enumerationReady || runtime.reconcilingOutputs ||
      runtime.readyNotified || !runtime.error.empty()) {
    return;
  }
  for (const auto &[id, filter] : runtime.filters) {
    static_cast<void>(id);
    if (!outputFilterSettled(*filter)) {
      return;
    }
  }
  runtime.readyNotified = true;
  if (runtime.timeoutSource != nullptr) {
    static_cast<void>(pw_loop_update_timer(
        pw_main_loop_get_loop(runtime.mainLoop), runtime.timeoutSource,
        nullptr, nullptr, false));
  }
  if (runtime.options.readyCallback != nullptr) {
    runtime.options.readyCallback(runtime.options.readyUserData);
  }
  if (runtime.mode == PipeWireRunMode::untilReady) {
    runtime.completed = true;
    pw_main_loop_quit(runtime.mainLoop);
  }
}

static void reconcileOutputFilters(FilterServiceRuntime &runtime) {
  if (!runtime.enumerationReady || runtime.reconcilingOutputs) {
    return;
  }
  runtime.reconcilingOutputs = true;
  auto desired = std::unordered_map<std::uint32_t, TransparentFilterOutput>{};
  for (const auto &output : runtime.outputTracker.outputs()) {
    desired.emplace(output.id, output);
  }

  for (auto iterator = runtime.filters.begin();
       iterator != runtime.filters.end();) {
    const auto found = desired.find(iterator->first);
    if (found != desired.end() && iterator->second->output == found->second) {
      ++iterator;
      continue;
    }
    retireOutputFilter(runtime, *iterator->second);
    iterator = runtime.filters.erase(iterator);
  }
  for (const auto &[id, output] : desired) {
    if (!runtime.filters.contains(id)) {
      runtime.filters.emplace(id, createOutputFilter(runtime, output));
    }
  }
  runtime.reconcilingOutputs = false;
  for (auto &[id, filter] : runtime.filters) {
    static_cast<void>(id);
    publishFilterEnabled(*filter);
  }
  maybeCompleteFilterServiceReadiness(runtime);
}

static void updateTrackedCandidate(PhysicalOutputNode &tracked) {
  auto &runtime = *tracked.runtime;
  if (runtime.outputTracker.update(tracked.candidate) &&
      runtime.enumerationReady) {
    reconcileOutputFilters(runtime);
  }
}

static void physicalNodeParameter(void *data, int, std::uint32_t id,
                                  std::uint32_t index, std::uint32_t,
                                  const spa_pod *parameter) {
  auto &tracked = *static_cast<PhysicalOutputNode *>(data);
  if (id == SPA_PARAM_EnumFormat) {
    tracked.candidate.sampleRateCapabilities =
        accumulatePipeWireSampleRateCapabilities(
            parameter, index, tracked.pendingConstraints);
    updateTrackedCandidate(tracked);
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
  tracked.candidate.activeSampleRate = activeRate;
  updateTrackedCandidate(tracked);
}

static void physicalNodeInfo(void *data, const pw_node_info *info) {
  auto &tracked = *static_cast<PhysicalOutputNode *>(data);
  if (info == nullptr) {
    return;
  }
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0 &&
      info->props != nullptr) {
    tracked.candidate =
        mergeCandidateProperties(tracked.candidate, info->props);
    updateTrackedCandidate(tracked);
  }
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) == 0) {
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
    tracked.candidate.sampleRateCapabilities = {};
    tracked.candidate.activeSampleRate = 0;
    updateTrackedCandidate(tracked);
    tracked.parameterAvailability = {};
    return;
  }
  if (availability.enumFormatReadable &&
      !tracked.parameterAvailability.enumFormatReadable) {
    if (pw_node_enum_params(
            tracked.node, 1, SPA_PARAM_EnumFormat, 0,
            std::numeric_limits<std::uint32_t>::max(), nullptr) < 0) {
      tracked.candidate.sampleRateCapabilities = {};
      updateTrackedCandidate(tracked);
    }
  }
  if (availability.formatReadable &&
      !tracked.parameterAvailability.formatReadable) {
    if (pw_node_enum_params(
            tracked.node, 2, SPA_PARAM_Format, 0,
            std::numeric_limits<std::uint32_t>::max(), nullptr) < 0) {
      tracked.candidate.activeSampleRate = 0;
      updateTrackedCandidate(tracked);
    }
  } else if (!availability.formatReadable) {
    tracked.candidate.activeSampleRate = 0;
    updateTrackedCandidate(tracked);
  }
  tracked.parameterAvailability = availability;
}

static void bindPhysicalOutputNode(FilterServiceRuntime &runtime,
                                   std::uint32_t id,
                                   std::uint32_t version,
                                   const spa_dict *properties) {
  auto tracked = std::make_unique<PhysicalOutputNode>();
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
  tracked->events.info = physicalNodeInfo;
  tracked->events.param = physicalNodeParameter;
  tracked->listener = {};
  tracked->candidate = candidateFromProperties(id, properties);
  tracked->pendingConstraints = {};
  tracked->parameterAvailability = {};
  tracked->listenerInstalled = false;
  const auto listenerResult = pw_node_add_listener(
      tracked->node, &tracked->listener, &tracked->events, tracked.get());
  if (listenerResult < 0) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(tracked->node));
    return;
  }
  tracked->listenerInstalled = true;
  runtime.outputTracker.update(tracked->candidate);
  runtime.physicalNodes.emplace(id, std::move(tracked));
}

static void bindPolicyMetadata(FilterServiceRuntime &runtime,
                               std::uint32_t id,
                               std::uint32_t version) {
  if (runtime.policyMetadata != nullptr) {
    return;
  }
  runtime.policyMetadata = static_cast<pw_metadata *>(pw_registry_bind(
      runtime.registry, id, PW_TYPE_INTERFACE_Metadata,
      std::min(version, static_cast<std::uint32_t>(PW_VERSION_METADATA)), 0));
  if (runtime.policyMetadata == nullptr) {
    return;
  }
  runtime.policyMetadataId = id;
  runtime.policyMetadataEvents = {};
  runtime.policyMetadataEvents.version = PW_VERSION_METADATA_EVENTS;
  runtime.policyMetadataEvents.property = policyMetadataProperty;
  const auto listenerResult = pw_metadata_add_listener(
      runtime.policyMetadata, &runtime.policyMetadataListener,
      &runtime.policyMetadataEvents, &runtime);
  if (listenerResult < 0) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.policyMetadata));
    runtime.policyMetadata = nullptr;
    runtime.policyMetadataId = PW_ID_ANY;
  }
}

static void filterRegistryGlobal(void *data, std::uint32_t id,
                                 std::uint32_t, const char *type,
                                 std::uint32_t version,
                                 const spa_dict *properties) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  if (type == nullptr) {
    return;
  }
  if (std::string_view(type) == PW_TYPE_INTERFACE_Metadata &&
      dictionaryString(properties, PW_KEY_METADATA_NAME) ==
          "pipetune-policy") {
    bindPolicyMetadata(runtime, id, version);
    return;
  }
  const auto nodeName = dictionaryString(properties, PW_KEY_NODE_NAME);
  if (std::string_view(type) != PW_TYPE_INTERFACE_Node ||
      dictionaryString(properties, PW_KEY_MEDIA_CLASS) != "Audio/Sink" ||
      filterBooleanValue(dictionaryValue(properties, "pipetune.filter")) ||
      std::string_view(nodeName).starts_with("pipetune.filter.")) {
    return;
  }
  bindPhysicalOutputNode(runtime, id, version, properties);
}

static void destroyPhysicalOutputNode(PhysicalOutputNode &tracked) {
  if (tracked.listenerInstalled) {
    spa_hook_remove(&tracked.listener);
    tracked.listenerInstalled = false;
  }
  if (tracked.node != nullptr) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(tracked.node));
    tracked.node = nullptr;
  }
}

static void filterRegistryGlobalRemoved(void *data, std::uint32_t id) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  if (id == runtime.policyMetadataId) {
    if (runtime.policyMetadata != nullptr) {
      spa_hook_remove(&runtime.policyMetadataListener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.policyMetadata));
    }
    runtime.policyMetadata = nullptr;
    runtime.policyMetadataId = PW_ID_ANY;
    runtime.policyProtocol.clear();
    runtime.policyBackend.clear();
    runtime.policyState.clear();
    refreshPolicyReady(runtime);
    return;
  }
  const auto found = runtime.physicalNodes.find(id);
  if (found == runtime.physicalNodes.end()) {
    return;
  }
  destroyPhysicalOutputNode(*found->second);
  runtime.physicalNodes.erase(found);
  runtime.outputTracker.remove(id);
  if (runtime.enumerationReady) {
    reconcileOutputFilters(runtime);
  }
}

static void filterCoreDone(void *data, std::uint32_t id, int sequence) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  if (id != PW_ID_CORE || sequence != runtime.enumerationSequence ||
      runtime.enumerationReady) {
    return;
  }
  if (!runtime.initialBindingsSynchronized) {
    // Registry globals contain only a property subset. Bind requests issued
    // from those callbacks are later than the first sync request, so a second
    // round trip is required before node info can classify physical outputs.
    runtime.initialBindingsSynchronized = true;
    runtime.enumerationSequence =
        pw_core_sync(runtime.core, PW_ID_CORE, sequence);
    if (runtime.enumerationSequence < 0) {
      failFilterService(
          runtime,
          filterSystemError("cannot synchronize PipeWire output details",
                            runtime.enumerationSequence));
    }
    return;
  }
  runtime.enumerationReady = true;
  reconcileOutputFilters(runtime);
}

static void filterCoreError(void *data, std::uint32_t id, int, int result,
                            const char *message) {
  if (id != PW_ID_CORE) {
    return;
  }
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  failFilterService(
      runtime,
      message == nullptr
          ? filterSystemError("PipeWire core failed", result)
          : "PipeWire core failed: " + std::string(message));
}

static void filterServiceInterrupted(void *data, int) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  runtime.completed = true;
  pw_main_loop_quit(runtime.mainLoop);
}

static void filterServiceReadinessTimedOut(void *data, std::uint64_t) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(data);
  if (!runtime.enumerationReady) {
    failFilterService(runtime,
                      "timed out while enumerating PipeWire outputs");
    return;
  }
  for (auto &[id, filter] : runtime.filters) {
    static_cast<void>(id);
    if (!outputFilterSettled(*filter)) {
      markOutputFilterFailed(
          *filter, "timed out while negotiating this output filter");
    }
  }
  maybeCompleteFilterServiceReadiness(runtime);
}

static std::optional<ControlRuntimeStatus> filterControlStatusSnapshot(
    FilterServiceRuntime &runtime) {
  auto lock = std::scoped_lock(runtime.controlStatusMutex);
  return runtime.controlStatusSnapshot;
}

static ControlMessageResult closeFilterControlResponse(
    std::string response, bool publishStatus) {
  return {.response = std::move(response),
          .connectionMode = ControlConnectionMode::close,
          .publishStatus = publishStatus};
}

static std::string provideFilterControlStatus(void *userData) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(userData);
  const auto status = filterControlStatusSnapshot(runtime);
  return status.has_value()
             ? makeControlStatusEvent(*status)
             : makeControlErrorResponse(
                   "transparent-filter status is not available");
}

static ControlMessageResult handleFilterControlRequest(
    std::string_view message, void *userData) {
  auto &runtime = *static_cast<FilterServiceRuntime *>(userData);
  const auto request = parseControlRequest(message);
  if (!request.error.empty()) {
    return closeFilterControlResponse(
        makeControlErrorResponse(request.error), false);
  }
  if (request.request.command == ControlCommand::subscribe) {
    return {.response = provideFilterControlStatus(&runtime),
            .connectionMode = ControlConnectionMode::subscribe,
            .publishStatus = false};
  }
  if (request.request.command != ControlCommand::status) {
    return closeFilterControlResponse(
        makeControlErrorResponse(
            "this transparent-filter control operation is not implemented"),
        false);
  }
  const auto status = filterControlStatusSnapshot(runtime);
  if (!status.has_value()) {
    return closeFilterControlResponse(
        makeControlErrorResponse(
            "transparent-filter status is not available"),
        false);
  }
  return closeFilterControlResponse(
      makeControlSuccessResponse(
          *status, std::span<const ControlWarning>{}),
      false);
}

static bool createFilterControlServer(FilterServiceRuntime &runtime) {
  refreshControlStatusSnapshot(runtime);
  if (runtime.options.controlSocketPath.empty()) {
    return true;
  }
  const auto options = ControlServerOptions{
      .handler = handleFilterControlRequest,
      .statusProvider = provideFilterControlStatus,
      .userData = &runtime};
  auto started =
      startControlServer(runtime.options.controlSocketPath, options);
  if (started.server == nullptr) {
    runtime.error =
        "cannot start PipeTune control server: " + started.error;
    return false;
  }
  runtime.controlServer = std::move(started.server);
  return true;
}

static bool createFilterServiceLoop(FilterServiceRuntime &runtime) {
  runtime.mainLoop = pw_main_loop_new(nullptr);
  if (runtime.mainLoop == nullptr) {
    runtime.error =
        filterSystemError("cannot create PipeWire filter main loop", -errno);
    return false;
  }
  auto *loop = pw_main_loop_get_loop(runtime.mainLoop);
  if (runtime.mode == PipeWireRunMode::untilReady) {
    runtime.timeoutSource = pw_loop_add_timer(
        loop, filterServiceReadinessTimedOut, &runtime);
    if (runtime.timeoutSource == nullptr) {
      runtime.error = filterSystemError(
          "cannot create PipeWire filter readiness timer", -errno);
      return false;
    }
    auto delay = timespec{
        .tv_sec = kFilterReadinessTimeoutSeconds, .tv_nsec = 0};
    auto interval = timespec{.tv_sec = 0, .tv_nsec = 0};
    const auto timerResult = pw_loop_update_timer(
        loop, runtime.timeoutSource, &delay, &interval, false);
    if (timerResult < 0) {
      runtime.error = filterSystemError(
          "cannot arm PipeWire filter readiness timer", timerResult);
      return false;
    }
  } else {
    runtime.interruptSource =
        pw_loop_add_signal(loop, SIGINT, filterServiceInterrupted, &runtime);
    runtime.terminateSource =
        pw_loop_add_signal(loop, SIGTERM, filterServiceInterrupted, &runtime);
    if (runtime.interruptSource == nullptr ||
        runtime.terminateSource == nullptr) {
      runtime.error = filterSystemError(
          "cannot install PipeWire filter signal handlers", -errno);
      return false;
    }
  }
  runtime.context = pw_context_new(loop, nullptr, 0);
  if (runtime.context == nullptr) {
    runtime.error =
        filterSystemError("cannot create PipeWire filter context", -errno);
    return false;
  }
  runtime.core = pw_context_connect(runtime.context, nullptr, 0);
  if (runtime.core == nullptr) {
    runtime.error =
        filterSystemError("cannot connect PipeWire filter core", -errno);
    return false;
  }
  runtime.coreEvents.version = PW_VERSION_CORE_EVENTS;
  runtime.coreEvents.done = filterCoreDone;
  runtime.coreEvents.error = filterCoreError;
  const auto coreListenerResult = pw_core_add_listener(
      runtime.core, &runtime.coreListener, &runtime.coreEvents, &runtime);
  if (coreListenerResult < 0) {
    runtime.error = filterSystemError(
        "cannot monitor PipeWire filter core", coreListenerResult);
    return false;
  }
  runtime.registry =
      pw_core_get_registry(runtime.core, PW_VERSION_REGISTRY, 0);
  if (runtime.registry == nullptr) {
    runtime.error =
        filterSystemError("cannot access PipeWire filter registry", -errno);
    return false;
  }
  runtime.registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
  runtime.registryEvents.global = filterRegistryGlobal;
  runtime.registryEvents.global_remove = filterRegistryGlobalRemoved;
  const auto registryListenerResult = pw_registry_add_listener(
      runtime.registry, &runtime.registryListener, &runtime.registryEvents,
      &runtime);
  if (registryListenerResult < 0) {
    runtime.error = filterSystemError(
        "cannot monitor PipeWire filter registry", registryListenerResult);
    return false;
  }
  runtime.enumerationSequence = pw_core_sync(runtime.core, PW_ID_CORE, 0);
  if (runtime.enumerationSequence < 0) {
    runtime.error = filterSystemError(
        "cannot synchronize PipeWire filter enumeration",
        runtime.enumerationSequence);
    return false;
  }
  return createFilterControlServer(runtime);
}

static void destroyFilterService(FilterServiceRuntime &runtime) {
  runtime.controlServer.reset();
  for (auto &[id, filter] : runtime.filters) {
    static_cast<void>(id);
    retireOutputFilter(runtime, *filter);
  }
  runtime.filters.clear();
  if (runtime.policyMetadata != nullptr) {
    spa_hook_remove(&runtime.policyMetadataListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.policyMetadata));
    runtime.policyMetadata = nullptr;
  }
  for (auto &[id, tracked] : runtime.physicalNodes) {
    static_cast<void>(id);
    destroyPhysicalOutputNode(*tracked);
  }
  runtime.physicalNodes.clear();
  if (runtime.registry != nullptr) {
    spa_hook_remove(&runtime.registryListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.registry));
    runtime.registry = nullptr;
  }
  if (runtime.core != nullptr) {
    spa_hook_remove(&runtime.coreListener);
    pw_core_disconnect(runtime.core);
    runtime.core = nullptr;
  }
  if (runtime.context != nullptr) {
    pw_context_destroy(runtime.context);
    runtime.context = nullptr;
  }
  if (runtime.mainLoop != nullptr) {
    auto *loop = pw_main_loop_get_loop(runtime.mainLoop);
    if (runtime.timeoutSource != nullptr) {
      pw_loop_destroy_source(loop, runtime.timeoutSource);
    }
    if (runtime.interruptSource != nullptr) {
      pw_loop_destroy_source(loop, runtime.interruptSource);
    }
    if (runtime.terminateSource != nullptr) {
      pw_loop_destroy_source(loop, runtime.terminateSource);
    }
    pw_main_loop_destroy(runtime.mainLoop);
    runtime.mainLoop = nullptr;
  }
}

static std::vector<PipeWireFilterOutputStatus>
filterServiceStatuses(const FilterServiceRuntime &runtime) {
  auto statuses = std::vector<PipeWireFilterOutputStatus>{};
  statuses.reserve(runtime.outputTracker.outputs().size() +
                   runtime.outputTracker.rejectedOutputs().size());
  for (const auto &output : runtime.outputTracker.outputs()) {
    auto state = outputFilterState(runtime, output);
    statuses.push_back(
        {.targetNodeName = output.nodeName,
         .targetDescription = output.description,
         .filterNodeName = output.filterNodeName,
         .state = state.state,
         .error = std::move(state.error),
         .channelCount = output.channelCount,
         .dspSampleRate = output.rates.dspSampleRate,
         .outputSampleRate = output.rates.outputSampleRate,
         .latencyFrames = state.latencyFrames});
  }
  for (const auto &rejected : runtime.outputTracker.rejectedOutputs()) {
    statuses.push_back({.targetNodeName = rejected.nodeName,
                        .targetDescription = rejected.description,
                        .filterNodeName = {},
                        .state = rejectedOutputState(rejected),
                        .error = rejected.error,
                        .channelCount = 0,
                        .dspSampleRate = 0,
                        .outputSampleRate = 0,
                        .latencyFrames = 0});
  }
  return statuses;
}

static PipeWireFilterServiceResult filterServiceValidationError(
    std::string error) {
  return {.success = false,
          .error = std::move(error),
          .policyBackend = {},
          .outputs = {},
          .overrunFrames = 0,
          .underrunFrames = 0,
          .processingErrors = 0};
}

static std::string validateFilterServiceOptions(
    const DspPipeline &pipeline,
    const PipeWireFilterServiceOptions &options) {
  if (!sampleRatePolicyIsValid(options.ratePolicy)) {
    return "sample-rate policy is invalid";
  }
  if (options.maxFrames < 32 ||
      options.maxFrames >
          static_cast<std::uint32_t>(INT_MAX) / kFilterSampleBytes) {
    return "PipeWire maximum frame count is outside its supported range";
  }
  if (options.ringCapacityFrames < options.maxFrames) {
    return "PipeWire ring capacity must be at least the maximum frame count";
  }
  if (options.initialPresetPath.string().find('\0') != std::string::npos ||
      options.controlSocketPath.string().find('\0') != std::string::npos) {
    return "preset and control socket paths must not contain NUL";
  }
  if (pipeline.maxFrames() < 32 || pipeline.maxChannels() == 0) {
    return "source DSP pipeline is invalid";
  }
  return {};
}

PipeWireFilterServiceResult runPipeWireFilterService(
    std::unique_ptr<DspPipeline> pipeline,
    const PipeWireFilterServiceOptions &options, PipeWireRunMode mode) {
  if (pipeline == nullptr) {
    return filterServiceValidationError("DSP pipeline must not be null");
  }
  const auto validation = validateFilterServiceOptions(*pipeline, options);
  if (!validation.empty()) {
    return filterServiceValidationError(validation);
  }
  try {
    auto library = FilterServicePipeWireScope{};
    auto runtime = FilterServiceRuntime(std::move(pipeline), options, mode);
    if (createFilterServiceLoop(runtime)) {
      const auto runResult = pw_main_loop_run(runtime.mainLoop);
      if (runResult < 0 && runtime.error.empty()) {
        failFilterService(
            runtime,
            filterSystemError("PipeWire filter main loop failed", runResult));
      } else if (!runtime.completed && runtime.error.empty()) {
        failFilterService(
            runtime, "PipeWire filter main loop stopped before completion");
      }
    }
    auto statuses = filterServiceStatuses(runtime);
    auto overrunFrames = runtime.retiredOverrunFrames;
    auto underrunFrames = runtime.retiredUnderrunFrames;
    auto processingErrors = runtime.retiredProcessingErrors;
    for (const auto &[id, filter] : runtime.filters) {
      static_cast<void>(id);
      overrunFrames += filter->ring.overrunFrames();
      underrunFrames += filter->ring.underrunFrames();
      processingErrors +=
          filter->processingErrors.load(std::memory_order_relaxed);
    }
    const auto success = runtime.completed && runtime.error.empty();
    auto error = runtime.error;
    auto policyBackend = runtime.policyBackend;
    destroyFilterService(runtime);
    return {.success = success,
            .error = std::move(error),
            .policyBackend = std::move(policyBackend),
            .outputs = std::move(statuses),
            .overrunFrames = overrunFrames,
            .underrunFrames = underrunFrames,
            .processingErrors = processingErrors};
  } catch (const std::exception &error) {
    return filterServiceValidationError(
        std::string("cannot prepare PipeWire filter service: ") +
        error.what());
  }
}

} // namespace pipetune
