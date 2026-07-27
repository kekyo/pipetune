#ifndef PIPETUNE_AUTOSTART_OVERRIDE_H
#define PIPETUNE_AUTOSTART_OVERRIDE_H

#include <filesystem>
#include <string>
#include <vector>

namespace pipetune {

/**
 * Reports an XDG autostart override update.
 */
struct AutostartUpdateResult {
  /** True when the requested safe state was reached. */
  bool success;
  /** Non-fatal unmanaged or orphaned override diagnostics. */
  std::vector<std::string> warnings;
  /** Fatal filesystem diagnostic. */
  std::string error;
};

/**
 * Checks whether a desktop file is PipeTune's managed Hidden mask.
 *
 * @param path Desktop file path.
 * @return True only for a readable file containing PipeTune's marker.
 */
bool isPipeTuneManagedAutostartMask(
    const std::filesystem::path &path);

/**
 * Installs a user XDG autostart mask, backing up a custom override.
 *
 * @param target User override path matching the system desktop filename.
 * @param backup Non-desktop backup path reserved for PipeTune.
 * @return Update outcome.
 */
AutostartUpdateResult
maskGtkAutostart(const std::filesystem::path &target,
                 const std::filesystem::path &backup);

/**
 * Removes PipeTune's mask and restores a backed-up custom override.
 *
 * Unmanaged target files and orphaned backups are preserved with warnings.
 *
 * @param target User override path matching the system desktop filename.
 * @param backup Non-desktop backup path reserved for PipeTune.
 * @return Update outcome.
 */
AutostartUpdateResult
restoreGtkAutostart(const std::filesystem::path &target,
                    const std::filesystem::path &backup);

} // namespace pipetune

#endif
