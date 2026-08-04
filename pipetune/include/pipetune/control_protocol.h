#ifndef PIPETUNE_CONTROL_PROTOCOL_H
#define PIPETUNE_CONTROL_PROTOCOL_H

#include "pipetune/dsp_backend.h"
#include "pipetune/sample_rate.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune {

/** Current local control protocol version. */
inline constexpr auto kControlProtocolVersion = std::uint32_t{2};

/**
 * Identifies one local control request.
 */
enum class ControlCommand {
  /** Return the running pipeline status. */
  status,
  /** Build and activate a preset without stopping audio. */
  loadPreset,
  /** Replace the active DSP pipeline with transparent pass-through. */
  bypass,
  /** Replace the configured DSP and graph-rate policy. */
  setRate,
  /** Rebuild the active preset with another native DSP backend. */
  setDspBackend,
  /** Keep the connection open and publish status changes. */
  subscribe
};

/**
 * Holds one validated control request.
 */
struct ControlRequest {
  /** Requested operation. */
  ControlCommand command;
  /** Preset path for loadPreset, or empty for other commands. */
  std::filesystem::path presetPath;
  /** Requested policy for setRate, or the default policy otherwise. */
  SampleRatePolicy ratePolicy = {};
  /** Requested backend for setDspBackend, or scalar otherwise. */
  DspBackendKind dspBackend = DspBackendKind::scalar;
  /** Requested SIMD dispatch preference for setDspBackend. */
  DspSimdVariant dspSimdVariant = DspSimdVariant::automatic;
};

/**
 * Reports a parsed request or one protocol diagnostic.
 */
struct ControlRequestParseResult {
  /** Parsed request; meaningful only when error is empty. */
  ControlRequest request;
  /** Protocol diagnostic, or empty after successful parsing. */
  std::string error;
};

/**
 * Identifies how the daemon currently handles captured PCM.
 */
enum class ProcessingMode {
  /** Forward PCM without invoking an EffeTune DSP engine. */
  bypass,
  /** Process PCM through the active preset pipeline. */
  preset
};

/**
 * Describes one packaged DSP backend in a runtime status response.
 */
struct ControlDspBackendAvailability {
  /** Backend variant. */
  DspBackendKind kind;
  /** True when CPU checks, loading, and ABI validation succeeded. */
  bool available;
  /** CPU feature level required on the running architecture. */
  std::string cpuRequirement;
  /** Availability diagnostic, or empty when available is true. */
  std::string error;
};

/**
 * Describes one concrete packaged DSP backend variant.
 */
struct ControlDspVariantAvailability {
  /** Concrete instruction-set variant. */
  DspBackendVariant variant;
  /** True when loading and ABI validation succeeded. */
  bool available;
  /** True when the running CPU satisfies the variant requirement. */
  bool cpuSupported;
  /** CPU feature level required on the running architecture. */
  std::string cpuRequirement;
  /** Availability diagnostic, or empty when available is true. */
  std::string error;
};

/** Identifies how one physical output is handled by PipeTune. */
enum class ControlFilterState {
  /** The per-output streams are still negotiating. */
  waiting,
  /** WirePlumber routes this output through its ready PipeTune filter. */
  active,
  /** The physical output remains on WirePlumber's direct route. */
  bypassed,
  /** The output-specific PipeTune runtime failed open. */
  error
};

/** Describes one physical output and its independent filter runtime. */
struct ControlFilterOutputStatus {
  /** Physical sink node.name that remains visible to the desktop. */
  std::string targetNodeName;
  /** User-facing physical sink description. */
  std::string targetDescription;
  /** Hidden PipeTune node.name, or empty when no filter was created. */
  std::string filterNodeName;
  /** Current target-specific filter state. */
  ControlFilterState state;
  /** Output-specific fail-open diagnostic, or empty. */
  std::string error;
  /** Exact number of processed channels, or zero for a direct route. */
  std::uint32_t channelCount;
  /** Enumerated rate support of this physical sink. */
  SampleRateCapabilities sampleRateCapabilities;
  /** EffeTune processing rate for this output, or zero. */
  std::uint32_t dspSampleRate;
  /** PipeWire graph-rate hint for this output, or zero. */
  std::uint32_t outputSampleRate;
  /** Active physical output rate, or zero while idle. */
  std::uint32_t activeOutputSampleRate;
  /** True when outputSampleRate is a device-compatible fallback. */
  bool rateFallback;
  /** Native DSP latency published to PipeWire, in frames. */
  std::uint32_t latencyFrames;
  /** Captured frames discarded because this output bridge was full. */
  std::uint64_t overrunFrames;
  /** Playback frames replaced by silence because this bridge was empty. */
  std::uint64_t underrunFrames;
  /** DSP blocks passed through after output-specific processing errors. */
  std::uint64_t processingErrors;
  /** Frames processed by this output's native EffeTune pipeline. */
  std::uint64_t dspProcessedFrames;
  /** Nanoseconds spent in this output's native EffeTune pipeline. */
  std::uint64_t dspProcessingNanoseconds;
};

/**
 * Describes the current daemon state returned over the control socket.
 */
struct ControlRuntimeStatus {
  /** Current PCM processing mode. */
  ProcessingMode processingMode;
  /** Active preset path, or empty while processingMode is bypass. */
  std::string activePreset;
  /** Startup configuration diagnostic, or empty when configuration is valid. */
  std::string configurationError;
  /** Number of enabled native DSP nodes. */
  std::size_t activePluginCount;
  /** WirePlumber compatibility backend, or empty without a handshake. */
  std::string policyBackend = {};
  /** Independently filtered or directly routed physical outputs. */
  std::vector<ControlFilterOutputStatus> filterOutputs = {};
  /** Input frames discarded because the bridge was full. */
  std::uint64_t overrunFrames;
  /** Output frames replaced by silence because the bridge was empty. */
  std::uint64_t underrunFrames;
  /** DSP blocks that could not be processed. */
  std::uint64_t processingErrors;
  /** Frames passed to the native EffeTune engine. */
  std::uint64_t dspProcessedFrames;
  /** Nanoseconds spent inside native EffeTune pipeline processing. */
  std::uint64_t dspProcessingNanoseconds;
  /** Persisted Max/fixed and suggest/force selection. */
  SampleRatePolicy configuredRatePolicy = {};
  /** Persisted or successfully applied DSP backend choice. */
  DspBackendKind configuredDspBackend = DspBackendKind::scalar;
  /** Persisted or successfully applied SIMD dispatch preference. */
  DspSimdVariant configuredDspSimdVariant = DspSimdVariant::automatic;
  /** Backend used by active presets, or no value without a usable scalar SO. */
  std::optional<DspBackendKind> effectiveDspBackend =
      DspBackendKind::scalar;
  /** Concrete backend used by active presets, or no value when unavailable. */
  std::optional<DspBackendVariant> effectiveDspVariant =
      DspBackendVariant::scalar;
  /** True when a lower tier or scalar replaced the preferred SIMD tier. */
  bool dspBackendFallback = false;
  /** DSP backend selection diagnostic, or empty without fallback. */
  std::string dspBackendError = {};
  /** Scalar and SIMD availability in stable order. */
  std::array<ControlDspBackendAvailability, 2> availableDspBackends = {{
      {.kind = DspBackendKind::scalar,
       .available = true,
       .cpuRequirement = "none",
       .error = {}},
      {.kind = DspBackendKind::simd,
       .available = false,
       .cpuRequirement = "unknown",
       .error = "SIMD DSP backend availability was not reported"},
  }};
  /** Concrete scalar and architecture-applicable SIMD variant availability. */
  std::vector<ControlDspVariantAvailability> availableDspVariants = {
      {.variant = DspBackendVariant::scalar,
       .available = true,
       .cpuSupported = true,
       .cpuRequirement = "none",
       .error = {}},
      {.variant = DspBackendVariant::simdBaseline,
       .available = false,
       .cpuSupported = false,
       .cpuRequirement = "unknown",
       .error = "SIMD DSP variant availability was not reported"}};
};

/**
 * Describes one preset node omitted while loading a pipeline.
 */
struct ControlWarning {
  /** Zero-based node index in the preset graph. */
  std::size_t nodeIndex;
  /** Preset plugin name. */
  std::string pluginName;
  /** Human-readable reason the node was omitted. */
  std::string reason;
};

/**
 * Identifies an ordinary reply or an asynchronous status publication.
 */
enum class ControlResponseKind {
  /** Reply to a status or load request. */
  response,
  /** Status published on a subscription connection. */
  statusEvent
};

/**
 * Reports a fully parsed control response.
 */
struct ControlResponseParseResult {
  /** True when all required protocol fields are valid. */
  bool valid;
  /** Value of the response's ok field when valid is true. */
  bool success;
  /** Ordinary response or subscribed status event. */
  ControlResponseKind kind;
  /** Runtime status for a successful response. */
  ControlRuntimeStatus status;
  /** Preset warnings for a successful response. */
  std::vector<ControlWarning> warnings;
  /** Remote diagnostic, or a local protocol diagnostic when invalid. */
  std::string error;
};

/**
 * Reports whether a JSON control response can be used by a client.
 */
struct ControlResponseInspection {
  /** True when the response has a valid protocol envelope. */
  bool valid;
  /** Value of the response's ok field when valid is true. */
  bool success;
  /** Remote diagnostic, or a local protocol diagnostic when invalid. */
  std::string error;
};

/**
 * Parses one complete JSON control request.
 *
 * @param json UTF-8 JSON object without framing newline.
 * @return Validated request or a protocol diagnostic.
 */
ControlRequestParseResult parseControlRequest(std::string_view json);

/** Returns a JSON status request without framing newline. */
std::string makeStatusControlRequest();

/** Returns a JSON subscription request without framing newline. */
std::string makeSubscribeControlRequest();

/** Returns a JSON live-bypass request without framing newline. */
std::string makeBypassControlRequest();

/**
 * Returns a JSON sample-rate-policy request without framing newline.
 *
 * @param policy Valid Max/fixed and suggest/force policy.
 * @return Encoded request, or an empty string for invalid input or failure.
 */
std::string makeSetRateControlRequest(const SampleRatePolicy &policy);

/**
 * Returns a JSON DSP-backend request without framing newline.
 *
 * @param kind Scalar compatibility or SIMD acceleration backend.
 * @return Encoded request, or an empty string for an invalid kind or failure.
 */
std::string makeSetDspBackendControlRequest(DspBackendKind kind);

/**
 * Returns a JSON DSP-backend and SIMD-variant request.
 *
 * @param kind Scalar compatibility or SIMD acceleration backend.
 * @param simdVariant Automatic or pinned SIMD dispatch preference.
 * @return Encoded request, or an empty string for invalid input or failure.
 */
std::string
makeSetDspBackendControlRequest(DspBackendKind kind,
                                DspSimdVariant simdVariant);

/**
 * Returns a JSON live-preset request without framing newline.
 *
 * @param presetPath Preset path interpreted by the running daemon.
 * @return Encoded request, or an empty string on allocation failure.
 */
std::string
makeLoadPresetControlRequest(const std::filesystem::path &presetPath);

/**
 * Returns a successful status response without framing newline.
 *
 * @param status Current daemon status.
 * @param warnings Nodes omitted while handling a load request.
 * @return Encoded JSON response.
 */
std::string makeControlSuccessResponse(
    const ControlRuntimeStatus &status,
    std::span<const ControlWarning> warnings);

/**
 * Returns a subscribed status event without framing newline.
 *
 * @param status Current daemon status.
 * @return Encoded JSON event.
 */
std::string makeControlStatusEvent(const ControlRuntimeStatus &status);

/**
 * Returns a failed response without framing newline.
 *
 * @param error Human-readable failure reason.
 * @return Encoded JSON response.
 */
std::string makeControlErrorResponse(std::string_view error);

/**
 * Parses and validates one complete server response or status event.
 *
 * @param json Complete JSON message without framing newline.
 * @return Parsed status, warnings, kind, and any diagnostic.
 */
ControlResponseParseResult parseControlResponse(std::string_view json);

/**
 * Validates the success envelope of one server response.
 *
 * @param json Complete JSON response without framing newline.
 * @return Envelope status and any remote or protocol diagnostic.
 */
ControlResponseInspection inspectControlResponse(std::string_view json);

} // namespace pipetune

#endif
