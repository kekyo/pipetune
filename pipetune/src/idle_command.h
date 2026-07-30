#ifndef PIPETUNE_IDLE_COMMAND_H
#define PIPETUNE_IDLE_COMMAND_H

#include "pipetune/control_protocol.h"

#include <filesystem>
#include <string>

namespace pipetune {

/**
 * Reports a live DSP and PipeWire idle status query.
 */
struct IdleStatusQueryResult {
  /** True when the daemon returned a valid successful status. */
  bool success;
  /** Parsed daemon DSP and PipeWire idle state. */
  ControlRuntimeStatus status;
  /** Original successful JSON response for machine-readable output. */
  std::string json;
  /** Transport, protocol, or daemon diagnostic. */
  std::string error;
};

/**
 * Selects the control and persistence endpoints for an idle policy change.
 */
struct PersistentIdleOptions {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Running daemon control socket path. */
  std::filesystem::path socketPath;
};

/**
 * Reports the live and persistence phases of an idle policy change.
 */
struct PersistentIdleResult {
  /** True when persistence and any live apply passed. */
  bool success;
  /** True when a running daemon confirmed the requested policy. */
  bool liveApplied;
  /** True when the startup configuration was updated. */
  bool persistenceApplied;
  /** Confirmed live status, or default state without a daemon. */
  ControlRuntimeStatus status;
  /** Non-fatal note when the policy is deferred until daemon startup. */
  std::string notice;
  /** Fatal or partial-success diagnostic. */
  std::string error;
};

/**
 * Queries the configured policy and current DSP/PipeWire idle state.
 *
 * @param socketPath Running daemon control socket path.
 * @return Parsed status, original JSON, or a diagnostic.
 */
IdleStatusQueryResult
queryIdleStatus(const std::filesystem::path &socketPath);

/**
 * Applies an idle policy live when possible, then persists it.
 *
 * An unavailable daemon is treated as an offline change. A daemon rejection
 * leaves persistence unchanged.
 *
 * @param options Resolved configuration and control paths.
 * @param policy Conservative threshold or exact-zero output policy.
 * @return Live and persistence outcomes.
 */
PersistentIdleResult
executeSetDspIdlePolicy(const PersistentIdleOptions &options,
                        DspIdlePolicy policy);

/**
 * Formats configured policy, runtime state, counters, and PipeWire idling.
 *
 * @param status Valid daemon status.
 * @return Human-readable status ending in a newline.
 */
std::string formatIdleStatus(const ControlRuntimeStatus &status);

} // namespace pipetune

#endif
