#ifndef PIPETUNE_STARTUP_CONFIG_H
#define PIPETUNE_STARTUP_CONFIG_H

#include "pipetune/sample_rate.h"

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
 * Reports all user choices stored in the startup configuration.
 */
struct StartupConfigLoadResult {
  /** True when the configuration contains a preset assignment. */
  bool presetFound;
  /** Absolute preset path when presetFound is true. */
  std::filesystem::path presetPath;
  /** True when the configuration contains an output preference. */
  bool preferredOutputFound;
  /** Preferred PipeWire node.name when preferredOutputFound is true. */
  std::string preferredOutput;
  /** User-selected DSP and PipeWire graph-rate policy. */
  SampleRatePolicy ratePolicy = {};
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
 * Loads the optional preset and preferred output from a configuration file.
 *
 * A missing file is successful with both found fields set to false.
 *
 * @param configPath Configuration file path.
 * @return Loaded user choices or a validation diagnostic.
 */
StartupConfigLoadResult
loadStartupConfig(const std::filesystem::path &configPath);

/**
 * Atomically stores an absolute startup preset while preserving the output
 * preference in a private configuration.
 *
 * @param configPath Configuration file path.
 * @param presetPath Absolute preset path to store.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath);

/**
 * Atomically stores intentional startup bypass while preserving the output
 * preference.
 *
 * @param configPath Configuration file path.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string clearStartupPreset(const std::filesystem::path &configPath);

/**
 * Atomically stores a preferred PipeWire output while preserving the preset.
 *
 * @param configPath Configuration file path.
 * @param nodeName Non-empty PipeWire node.name to store.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string savePreferredOutput(const std::filesystem::path &configPath,
                                std::string_view nodeName);

/**
 * Atomically removes the preferred output while preserving the preset.
 *
 * @param configPath Configuration file path.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string clearPreferredOutput(const std::filesystem::path &configPath);

/**
 * Atomically stores a sample-rate policy while preserving preset and output
 * choices.
 *
 * @param configPath Configuration file path.
 * @param policy Valid Max/fixed and suggest/force policy.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveSampleRatePolicy(const std::filesystem::path &configPath,
                                 const SampleRatePolicy &policy);

/**
 * Atomically replaces the startup configuration with PipeTune defaults.
 *
 * The stored defaults select DSP bypass, the system-default output, and the
 * Max + Suggest sample-rate policy. Existing contents are not parsed.
 *
 * @param configPath Configuration file path.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string resetStartupConfig(const std::filesystem::path &configPath);

} // namespace pipetune

#endif
