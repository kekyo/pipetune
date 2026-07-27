#ifndef PIPETUNE_BYPASS_COMMAND_H
#define PIPETUNE_BYPASS_COMMAND_H

#include <filesystem>
#include <string>

namespace pipetune {

/**
 * Selects the control and persistence endpoints for a bypass operation.
 */
struct PersistentBypassOptions {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Running daemon control socket path. */
  std::filesystem::path socketPath;
};

/**
 * Reports the two phases of a persistent bypass operation.
 */
struct PersistentBypassResult {
  /** True when startup bypass was persisted without a live protocol failure. */
  bool success;
  /** True when the running daemon confirmed live bypass. */
  bool liveApplied;
  /** True when the startup configuration was cleared. */
  bool persistenceApplied;
  /** Non-fatal note, including daemon-unavailable deferred application. */
  std::string notice;
  /** Fatal or partial-success diagnostic. */
  std::string error;
};

/**
 * Switches a reachable daemon to bypass, then persists startup bypass.
 *
 * When the daemon cannot be reached, startup bypass is still persisted. A
 * daemon rejection or invalid daemon response leaves startup configuration
 * unchanged.
 *
 * @param options Resolved configuration and control paths.
 * @return Phase outcomes and diagnostics.
 */
PersistentBypassResult
executePersistentBypass(const PersistentBypassOptions &options);

} // namespace pipetune

#endif
