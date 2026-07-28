#ifndef PIPETUNE_OUTPUT_COMMAND_H
#define PIPETUNE_OUTPUT_COMMAND_H

#include "pipetune/control_protocol.h"

#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace pipetune {

/**
 * Reports a live output-status query.
 */
struct OutputStatusQueryResult {
  /** True when the daemon returned a valid successful status. */
  bool success;
  /** Parsed engine-owned output state. */
  ControlRuntimeStatus status;
  /** Original successful JSON response for machine-readable CLI output. */
  std::string json;
  /** Transport, protocol, or daemon diagnostic. */
  std::string error;
};

/**
 * Selects the control and persistence endpoints for an output change.
 */
struct PersistentOutputOptions {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Running daemon control socket path. */
  std::filesystem::path socketPath;
};

/**
 * Reports the live and persistence phases of an output change.
 */
struct PersistentOutputResult {
  /** True when both live application and persistence succeeded. */
  bool success;
  /** True when the running daemon confirmed the requested preference. */
  bool liveApplied;
  /** True when the startup configuration was updated. */
  bool persistenceApplied;
  /** Status returned after the live change. */
  ControlRuntimeStatus status;
  /** Fatal or partial-success diagnostic. */
  std::string error;
};

/**
 * Reports one choice from the interactive output menu.
 */
struct OutputSelectionChoice {
  /** True when a menu item was selected. */
  bool success;
  /** True when the user chose to clear the explicit preference. */
  bool clearPreference;
  /** Selected PipeWire node.name, or empty when clearPreference is true. */
  std::string target;
  /** Cancellation or input diagnostic. */
  std::string error;
};

/**
 * Queries the engine-owned list and effective output selection.
 *
 * @param socketPath Running daemon control socket path.
 * @return Parsed status, original JSON, or a diagnostic.
 */
OutputStatusQueryResult
queryOutputStatus(const std::filesystem::path &socketPath);

/**
 * Changes the running daemon first and persists the confirmed preference.
 *
 * A missing or rejecting daemon leaves the startup configuration unchanged.
 * If persistence fails after daemon confirmation, the live change remains
 * active and the result explicitly reports partial success.
 *
 * @param options Resolved configuration and control paths.
 * @param target Non-empty PipeWire node.name, which may currently be absent.
 * @return Phase outcomes, confirmed status, and any diagnostic.
 */
PersistentOutputResult
executeSetPreferredOutput(const PersistentOutputOptions &options,
                          std::string_view target);

/**
 * Clears the running daemon preference first, then persists that choice.
 *
 * @param options Resolved configuration and control paths.
 * @return Phase outcomes, confirmed status, and any diagnostic.
 */
PersistentOutputResult
executeClearPreferredOutput(const PersistentOutputOptions &options);

/**
 * Formats all selectable engine-reported outputs for a terminal.
 *
 * @param status Valid daemon status.
 * @return Human-readable device list ending in a newline.
 */
std::string formatOutputDeviceList(const ControlRuntimeStatus &status);

/**
 * Formats the preferred, effective, and fallback state for a terminal.
 *
 * @param status Valid daemon status.
 * @return Human-readable selection state ending in a newline.
 */
std::string formatOutputSelection(const ControlRuntimeStatus &status);

/**
 * Prompts for system-default mode or one engine-reported output.
 *
 * Invalid input is explained and prompted again. End-of-file cancels the
 * operation without changing either the daemon or persistent configuration.
 *
 * @param status Valid daemon status whose output ordering is displayed.
 * @param input Terminal input stream.
 * @param output Terminal output stream.
 * @return Selected preference, clear request, or cancellation diagnostic.
 */
OutputSelectionChoice
promptForOutputSelection(const ControlRuntimeStatus &status,
                         std::istream &input, std::ostream &output);

} // namespace pipetune

#endif
