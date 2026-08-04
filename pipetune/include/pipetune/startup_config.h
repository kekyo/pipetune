#ifndef PIPETUNE_STARTUP_CONFIG_H
#define PIPETUNE_STARTUP_CONFIG_H

#include "pipetune/dsp_backend.h"
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
 * Contains all user choices stored in the startup configuration.
 */
struct StartupConfig {
  /** True when the configuration contains a preset assignment. */
  bool presetFound = false;
  /** Absolute preset path when presetFound is true. */
  std::filesystem::path presetPath;
  /** User-selected DSP and PipeWire graph-rate policy. */
  SampleRatePolicy ratePolicy = {};
  /** User-selected native DSP backend. */
  DspBackendKind dspBackend = DspBackendKind::scalar;
  /** Automatic or pinned SIMD dispatch preference. */
  DspSimdVariant dspSimdVariant = DspSimdVariant::automatic;
};

/**
 * Reports all user choices loaded from the startup configuration.
 */
struct StartupConfigLoadResult {
  /** Loaded configuration, or defaults when loading fails. */
  StartupConfig config;
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
 * Loads the optional preset and processing settings from a configuration file.
 *
 * A missing file is successful with presetFound set to false.
 *
 * @param configPath Configuration file path.
 * @return Loaded user choices or a validation diagnostic.
 */
StartupConfigLoadResult
loadStartupConfig(const std::filesystem::path &configPath);

/**
 * Atomically replaces the startup configuration with one complete snapshot.
 *
 * @param configPath Configuration file path.
 * @param config Complete validated configuration to store.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveStartupConfig(const std::filesystem::path &configPath,
                              const StartupConfig &config);

/**
 * Atomically stores an absolute startup preset while preserving other
 * settings in a private configuration.
 *
 * @param configPath Configuration file path.
 * @param presetPath Absolute preset path to store.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath);

/**
 * Atomically stores intentional startup bypass while preserving other
 * settings.
 *
 * @param configPath Configuration file path.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string clearStartupPreset(const std::filesystem::path &configPath);

/**
 * Atomically stores a sample-rate policy while preserving other choices.
 *
 * @param configPath Configuration file path.
 * @param policy Valid Max/fixed and suggest/force policy.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveSampleRatePolicy(const std::filesystem::path &configPath,
                                 const SampleRatePolicy &policy);

/**
 * Atomically stores a DSP backend choice while preserving other choices.
 *
 * @param configPath Configuration file path.
 * @param kind Scalar compatibility or SIMD acceleration backend.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveDspBackendKind(const std::filesystem::path &configPath,
                               DspBackendKind kind);

/**
 * Atomically stores a DSP backend and SIMD dispatch preference.
 *
 * @param configPath Configuration file path.
 * @param kind Scalar compatibility or SIMD acceleration backend.
 * @param simdVariant Automatic or pinned SIMD dispatch preference.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string saveDspBackendSelection(
    const std::filesystem::path &configPath, DspBackendKind kind,
    DspSimdVariant simdVariant);

/**
 * Atomically replaces the startup configuration with PipeTune defaults.
 *
 * The stored defaults select DSP bypass, Max + Suggest, and the scalar DSP
 * backend. Existing contents are not parsed.
 *
 * @param configPath Configuration file path.
 * @return Empty on success, otherwise a human-readable diagnostic.
 */
std::string resetStartupConfig(const std::filesystem::path &configPath);

} // namespace pipetune

#endif
