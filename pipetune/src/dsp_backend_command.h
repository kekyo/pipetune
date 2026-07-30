#ifndef PIPETUNE_DSP_BACKEND_COMMAND_H
#define PIPETUNE_DSP_BACKEND_COMMAND_H

#include "pipetune/control_protocol.h"

#include <filesystem>
#include <string>

namespace pipetune {

/**
 * Reports a live DSP backend status query.
 */
struct DspBackendStatusQueryResult {
  /** True when the daemon returned a valid successful status. */
  bool success;
  /** Parsed daemon backend state. */
  ControlRuntimeStatus status;
  /** Original successful JSON response for machine-readable output. */
  std::string json;
  /** Transport, protocol, or daemon diagnostic. */
  std::string error;
};

/**
 * Selects the control and persistence endpoints for a backend change.
 */
struct PersistentDspBackendOptions {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Running daemon control socket path. */
  std::filesystem::path socketPath;
};

/**
 * Reports the live and persistence phases of a backend change.
 */
struct PersistentDspBackendResult {
  /** True when persistence and any required live switch succeeded. */
  bool success;
  /** True when a running daemon completed the switch. */
  bool liveApplied;
  /** True when the startup configuration was updated. */
  bool persistenceApplied;
  /** Confirmed live status, or default state while offline. */
  ControlRuntimeStatus status;
  /** Non-fatal note when the switch is deferred until daemon startup. */
  std::string notice;
  /** Fatal or partial-success diagnostic. */
  std::string error;
};

/**
 * Queries configured, effective, and available DSP backends.
 *
 * @param socketPath Running daemon control socket path.
 * @return Parsed status, original JSON, or a diagnostic.
 */
DspBackendStatusQueryResult
queryDspBackendStatus(const std::filesystem::path &socketPath);

/**
 * Applies a backend live when possible, then persists it.
 *
 * An unavailable daemon causes local SO, CPU, and ABI validation before
 * persistence. A daemon rejection leaves persistence unchanged.
 *
 * @param options Resolved configuration and control paths.
 * @param kind Scalar compatibility or SIMD acceleration backend.
 * @return Live and persistence outcomes.
 */
PersistentDspBackendResult
executeSetDspBackend(const PersistentDspBackendOptions &options,
                     DspBackendKind kind);

/**
 * Formats configured and effective backend state.
 *
 * @param status Valid daemon status.
 * @return Human-readable status ending in a newline.
 */
std::string formatDspBackendStatus(const ControlRuntimeStatus &status);

/**
 * Formats scalar and SIMD availability diagnostics.
 *
 * @param status Valid daemon status.
 * @return Human-readable list ending in a newline.
 */
std::string formatDspBackendList(const ControlRuntimeStatus &status);

} // namespace pipetune

#endif
