#ifndef PIPETUNE_CONTROL_PROTOCOL_H
#define PIPETUNE_CONTROL_PROTOCOL_H

#include "pipetune/dsp_pipeline.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace pipetune {

/**
 * Identifies one local control request.
 */
enum class ControlCommand {
  /** Return the running pipeline status. */
  status,
  /** Build and activate a preset without stopping audio. */
  loadPreset
};

/**
 * Holds one validated control request.
 */
struct ControlRequest {
  /** Requested operation. */
  ControlCommand command;
  /** Preset path for loadPreset, or empty for status. */
  std::filesystem::path presetPath;
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
 * Describes the current daemon state returned over the control socket.
 */
struct ControlRuntimeStatus {
  /** Active preset path as supplied by the user. */
  std::string activePreset;
  /** Number of enabled native DSP nodes. */
  std::size_t activePluginCount;
  /** Current physical PipeWire output node name, or empty while unavailable. */
  std::string selectedTarget;
  /** Input frames discarded because the bridge was full. */
  std::uint64_t overrunFrames;
  /** Output frames replaced by silence because the bridge was empty. */
  std::uint64_t underrunFrames;
  /** DSP blocks that could not be processed. */
  std::uint64_t processingErrors;
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
    std::span<const PipelineWarning> warnings);

/**
 * Returns a failed response without framing newline.
 *
 * @param error Human-readable failure reason.
 * @return Encoded JSON response.
 */
std::string makeControlErrorResponse(std::string_view error);

/**
 * Validates the success envelope of one server response.
 *
 * @param json Complete JSON response without framing newline.
 * @return Envelope status and any remote or protocol diagnostic.
 */
ControlResponseInspection inspectControlResponse(std::string_view json);

} // namespace pipetune

#endif
