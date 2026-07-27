#ifndef PIPETUNE_GTK_STARTUP_CONFIG_H
#define PIPETUNE_GTK_STARTUP_CONFIG_H

#include <filesystem>
#include <string>
#include <string_view>

namespace pipetune_gtk {

/**
 * Reports a resolved GTK-managed environment override path.
 */
struct StartupConfigPathResult {
  /** `$XDG_CONFIG_HOME/pipetune/environment.gtk` or its HOME fallback. */
  std::filesystem::path path;
  /** Resolution diagnostic, or empty on success. */
  std::string error;
};

/**
 * Reports the preset stored in the GTK-managed environment override.
 */
struct StartupPresetLoadResult {
  /** True when an override file and preset assignment exist. */
  bool found;
  /** Absolute preset path when found is true. */
  std::filesystem::path presetPath;
  /** Read or validation diagnostic, or empty on success. */
  std::string error;
};

/**
 * Resolves the GTK-managed systemd environment override.
 *
 * @param xdgConfigHome Value of XDG_CONFIG_HOME, or empty for HOME fallback.
 * @param homeDirectory Value of HOME.
 * @return Resolved path or a missing-directory diagnostic.
 */
StartupConfigPathResult
resolveStartupConfigPath(std::string_view xdgConfigHome,
                         const std::filesystem::path &homeDirectory);

/**
 * Loads a preset from a GTK-managed environment override.
 *
 * A missing file is successful with found set to false.
 *
 * @param configPath Override file path.
 * @return Loaded preset, missing state, or diagnostic.
 */
StartupPresetLoadResult
loadStartupPreset(const std::filesystem::path &configPath);

/**
 * Atomically stores the startup preset in a private environment override.
 *
 * @param configPath Override file path.
 * @param presetPath Absolute preset path to store.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath);

} // namespace pipetune_gtk

#endif
