#ifndef PIPETUNE_RATE_COMMAND_H
#define PIPETUNE_RATE_COMMAND_H

#include "pipetune/control_protocol.h"

#include <filesystem>
#include <string>

namespace pipetune {

/**
 * Reports a live sample-rate status query.
 */
struct RateStatusQueryResult {
  /** True when the daemon returned a valid successful status. */
  bool success;
  /** Parsed daemon sample-rate state. */
  ControlRuntimeStatus status;
  /** Original successful JSON response for machine-readable output. */
  std::string json;
  /** Transport, protocol, or daemon diagnostic. */
  std::string error;
};

/**
 * Selects the control and persistence endpoints for a rate change.
 */
struct PersistentRateOptions {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Running daemon control socket path. */
  std::filesystem::path socketPath;
};

/**
 * Reports the live and persistence phases of a sample-rate change.
 */
struct PersistentRateResult {
  /** True when the requested policy was persisted and any live apply passed. */
  bool success;
  /** True when a running daemon completed renegotiation. */
  bool liveApplied;
  /** True when the startup configuration was updated. */
  bool persistenceApplied;
  /** Confirmed live status, or default state when no daemon was available. */
  ControlRuntimeStatus status;
  /** Non-fatal note when the policy is deferred until daemon startup. */
  std::string notice;
  /** Fatal or partial-success diagnostic. */
  std::string error;
};

/**
 * Queries the configured and negotiated sample rates.
 *
 * @param socketPath Running daemon control socket path.
 * @return Parsed status, original JSON, or a diagnostic.
 */
RateStatusQueryResult
queryRateStatus(const std::filesystem::path &socketPath);

/**
 * Applies a policy live when possible, then persists it.
 *
 * An unavailable daemon is treated as an offline change and the policy is
 * saved for the next start. A daemon rejection leaves persistence unchanged.
 *
 * @param options Resolved configuration and control paths.
 * @param policy Valid automatic/fixed graph-rate choice.
 * @return Live and persistence outcomes.
 */
PersistentRateResult
executeSetSampleRatePolicy(const PersistentRateOptions &options,
                           const SampleRatePolicy &policy);

/**
 * Formats the configured and effective sample-rate state.
 *
 * @param status Valid daemon status.
 * @return Human-readable status ending in a newline.
 */
std::string formatSampleRateStatus(const ControlRuntimeStatus &status);

/**
 * Formats automatic graph negotiation and the selectable fixed rates.
 *
 * @return Human-readable selection list ending in a newline.
 */
std::string formatSelectableSampleRates();

} // namespace pipetune

#endif
