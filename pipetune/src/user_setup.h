#ifndef PIPETUNE_USER_SETUP_H
#define PIPETUNE_USER_SETUP_H

#include "process_runner.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune {

/**
 * Groups per-user state and installed executable paths.
 */
struct UserManagementPaths {
  /** Canonical startup configuration. */
  std::filesystem::path configPath;
  /** Obsolete GTK configuration removed only by unsetup --purge. */
  std::filesystem::path legacyConfigPath;
  /** User XDG autostart override matching PipeTune's desktop filename. */
  std::filesystem::path autostartPath;
  /** Non-desktop backup reserved for an existing custom override. */
  std::filesystem::path autostartBackupPath;
  /** WirePlumber 0.4 endpoint policy fragment managed by PipeTune. */
  std::filesystem::path wirePlumberPolicyPath;
  /** WirePlumber 0.4 application-to-endpoint policy managed by PipeTune. */
  std::filesystem::path wirePlumberClientScriptPath;
  /** WirePlumber 0.4 endpoint-to-device policy managed by PipeTune. */
  std::filesystem::path wirePlumberDeviceScriptPath;
  /** Absolute systemctl executable path. */
  std::filesystem::path systemctlExecutable;
  /** Absolute pipetune-gtk executable path. */
  std::filesystem::path gtkExecutable;
};

/**
 * Reports resolved management paths.
 */
struct UserManagementPathResult {
  /** Resolved paths, meaningful when error is empty. */
  UserManagementPaths paths;
  /** Resolution diagnostic. */
  std::string error;
};

/**
 * Resolves PipeTune's per-user configuration and autostart paths.
 *
 * @param xdgConfigHome Value of XDG_CONFIG_HOME, or empty for HOME fallback.
 * @param homeDirectory Value of HOME.
 * @param systemctlExecutable Installed systemctl path.
 * @param gtkExecutable Installed pipetune-gtk path.
 * @return Resolved paths or a diagnostic.
 */
UserManagementPathResult resolveUserManagementPaths(
    std::string_view xdgConfigHome,
    const std::filesystem::path &homeDirectory,
    const std::filesystem::path &systemctlExecutable,
    const std::filesystem::path &gtkExecutable);

/**
 * Injectable direct process runner used by setup coordination.
 */
using ProcessRunner = ProcessResult (*)(
    const std::filesystem::path &executable,
    std::span<const std::string> arguments, ProcessWaitMode mode,
    void *userData);

/**
 * Describes one setup request.
 */
struct UserSetupRequest {
  /** Effective user identifier; zero is rejected. */
  std::uint32_t effectiveUserId;
  /** True when presetPath was explicitly supplied. */
  bool presetSpecified;
  /** Absolute preset path when presetSpecified is true. */
  std::filesystem::path presetPath;
  /** Resolved state and executable paths. */
  UserManagementPaths paths;
  /** Non-null direct process runner. */
  ProcessRunner processRunner;
  /** Opaque process runner argument. */
  void *processUserData;
};

/**
 * Describes one unsetup request.
 */
struct UserUnsetupRequest {
  /** Effective user identifier; zero is rejected. */
  std::uint32_t effectiveUserId;
  /** True to delete PipeTune application configuration after service stop. */
  bool purge;
  /** Resolved state and executable paths. */
  UserManagementPaths paths;
  /** Non-null direct process runner. */
  ProcessRunner processRunner;
  /** Opaque process runner argument. */
  void *processUserData;
};

/**
 * Reports setup or unsetup completion.
 */
struct UserManagementResult {
  /** True when all required operations completed. */
  bool success;
  /** Non-fatal rollback, GTK, or override diagnostics. */
  std::vector<std::string> warnings;
  /** Fatal or partial-completion diagnostic. */
  std::string error;
};

/**
 * Configures and starts PipeTune for one non-root user.
 *
 * @param request Setup request and injected process runner.
 * @return Completion and rollback diagnostics.
 */
UserManagementResult executeUserSetup(const UserSetupRequest &request);

/**
 * Stops and disables PipeTune for one non-root user.
 *
 * @param request Unsetup request and injected external operations.
 * @return Completion and partial-state diagnostics.
 */
UserManagementResult executeUserUnsetup(
    const UserUnsetupRequest &request);

} // namespace pipetune

#endif
