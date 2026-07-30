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
  /** Replace the user-preferred physical output. */
  setOutput,
  /** Clear the user-preferred output and follow the system default. */
  clearOutput,
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
  /** Preferred PipeWire node.name for setOutput, or empty otherwise. */
  std::string outputTarget;
  /** Requested policy for setRate, or the default policy otherwise. */
  SampleRatePolicy ratePolicy = {};
  /** Requested backend for setDspBackend, or scalar otherwise. */
  DspBackendKind dspBackend = DspBackendKind::scalar;
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
 * Identifies why the engine selected its current physical output.
 */
enum class ControlOutputSelectionReason {
  /** No usable physical output is currently available. */
  unavailable,
  /** No preference exists and the physical system default is selected. */
  systemDefault,
  /** The available user-preferred output is selected. */
  preferred,
  /** The preference is unavailable and a physical fallback is selected. */
  fallback
};

/**
 * Describes one selectable physical PipeWire output.
 */
struct ControlOutputDevice {
  /** Stable PipeWire node.name used by setOutput. */
  std::string name;
  /** Human-readable device description. */
  std::string description;
  /** True when this node is the remembered physical system default. */
  bool systemDefault;
  /** True when this available node matches the user preference. */
  bool preferred;
  /** True when this node is the effective playback target. */
  bool selected;
  /** Enumerated output sample-rate support, or unknown before enumeration. */
  SampleRateCapabilities sampleRateCapabilities = {};
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
  /** User-preferred PipeWire node.name, or empty for system-default mode. */
  std::string preferredTarget;
  /** Current physical PipeWire output node name, or empty while unavailable. */
  std::string selectedTarget;
  /** Reason selectedTarget was chosen. */
  ControlOutputSelectionReason outputSelectionReason;
  /** Current selectable physical outputs. */
  std::vector<ControlOutputDevice> availableOutputs;
  /** True after PipeTune's virtual sink became the effective default. */
  bool defaultSinkActive;
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
  /** Negotiated PipeWire input sample format, or empty before negotiation. */
  std::string inputSampleFormat;
  /** Negotiated PipeWire input sample rate, or zero before negotiation. */
  std::uint32_t inputSampleRate;
  /** Negotiated PipeWire input channel count, or zero before negotiation. */
  std::uint32_t inputChannelCount;
  /** Total valid PCM frames received from PipeWire. */
  std::uint64_t inputFramesReceived;
  /** Unix time of the latest received frame in milliseconds, or zero before input. */
  std::uint64_t inputLastReceivedUnixMilliseconds;
  /** Persisted Max/fixed and suggest/force selection. */
  SampleRatePolicy configuredRatePolicy = {};
  /** Resolved capture, playback-media-format, and DSP rate in hertz. */
  std::uint32_t dspSampleRate = 0;
  /** Selected physical/graph output rate in hertz. */
  std::uint32_t selectedOutputSampleRate = 0;
  /** Active physical output rate, or zero while idle or unavailable. */
  std::uint32_t activeOutputSampleRate = 0;
  /** True while the daemon is renegotiating R and H. */
  bool rateTransitioning = false;
  /** True when H is a device-compatible fallback for the requested R. */
  bool rateFallback = false;
  /** Most recent automatic or live rate-transition diagnostic. */
  std::string rateError = {};
  /** Persisted or successfully applied DSP backend choice. */
  DspBackendKind configuredDspBackend = DspBackendKind::scalar;
  /** Backend used by active presets, or no value without a usable scalar SO. */
  std::optional<DspBackendKind> effectiveDspBackend =
      DspBackendKind::scalar;
  /** True when configured SIMD is unavailable and scalar is effective. */
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
 * Returns a JSON preferred-output request without framing newline.
 *
 * @param nodeName Non-empty PipeWire node.name interpreted by the daemon.
 * @return Encoded request, or an empty string on allocation failure.
 */
std::string makeSetOutputControlRequest(std::string_view nodeName);

/** Returns a JSON system-default output request without framing newline. */
std::string makeClearOutputControlRequest();

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
