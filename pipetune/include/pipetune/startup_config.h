#ifndef PIPETUNE_STARTUP_CONFIG_H
#define PIPETUNE_STARTUP_CONFIG_H

#include <filesystem>
#include <string>
#include <string_view>

namespace pipetune {

/**
 * Reports the canonical per-user PipeTune configuration path.
 */
struct StartupConfigPathResult {
  /** `$XDG_CONFIG_HOME/pipetune/environment` or its HOME fallback. */
  std::filesystem::path path;
  /** Resolution diagnostic, or empty on success. */
  std::string error;
};

/**
 * Reports the optional preset stored in the startup configuration.
 */
struct StartupPresetLoadResult {
  /** True when the configuration contains a preset assignment. */
  bool found;
  /** Absolute preset path when found is true. */
  std::filesystem::path presetPath;
  /** Read or validation diagnostic, or empty on success. */
  std::string error;
};

/**
 * Resolves the canonical per-user PipeTune configuration file.
 *
 * @param xdgConfigHome Value of XDG_CONFIG_HOME, or empty for HOME fallback.
 * @param homeDirectory Value of HOME.
 * @return Resolved path or a missing-directory diagnostic.
 */
StartupConfigPathResult
resolveStartupConfigPath(std::string_view xdgConfigHome,
                         const std::filesystem::path &homeDirectory);

/**
 * Loads the optional startup preset from a PipeTune configuration file.
 *
 * A missing file or a file without PIPETUNE_PRESET is successful with found
 * set to false.
 *
 * @param configPath Configuration file path.
 * @return Loaded preset, intentional bypass state, or diagnostic.
 */
StartupPresetLoadResult
loadStartupPreset(const std::filesystem::path &configPath);

/**
 * Atomically stores an absolute startup preset in a private configuration.
 *
 * @param configPath Configuration file path.
 * @param presetPath Absolute preset path to store.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath);

/**
 * Atomically stores an intentional startup-bypass configuration.
 *
 * @param configPath Configuration file path.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string clearStartupPreset(const std::filesystem::path &configPath);

} // namespace pipetune

#endif
