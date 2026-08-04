#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static void writeConfig(const std::filesystem::path &path,
                        std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream(path, std::ios::binary);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static pipetune::StartupConfig configuredSnapshot() {
  return {
      .presetFound = true,
      .presetPath = "/tmp/snapshot.effetune_preset",
      .ratePolicy =
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 192000,
           .enforcement = pipetune::SampleRateEnforcement::force},
      .dspBackend = pipetune::DspBackendKind::simd,
      .dspSimdVariant = pipetune::DspSimdVariant::x86_64_v3,
  };
}

static bool configMatches(const pipetune::StartupConfig &actual,
                          const pipetune::StartupConfig &expected) {
  return actual.presetFound == expected.presetFound &&
         actual.presetPath == expected.presetPath &&
         actual.ratePolicy == expected.ratePolicy &&
         actual.dspBackend == expected.dspBackend &&
         actual.dspSimdVariant == expected.dspSimdVariant;
}

static bool testPathResolution() {
  const auto explicitPath =
      pipetune::resolveStartupConfigPath("/tmp/xdg-config", "/tmp/home");
  const auto fallbackPath =
      pipetune::resolveStartupConfigPath({}, "/tmp/home");
  const auto unavailable = pipetune::resolveStartupConfigPath({}, {});
  return check(explicitPath.error.empty() &&
                   explicitPath.path ==
                       "/tmp/xdg-config/pipetune/environment",
               "XDG startup config path differs") &&
         check(fallbackPath.error.empty() &&
                   fallbackPath.path ==
                       "/tmp/home/.config/pipetune/environment",
               "fallback startup config path differs") &&
         check(!unavailable.error.empty(),
               "missing XDG_CONFIG_HOME and HOME must be rejected");
}

static bool testPrivatePresetRoundTrip(
    const std::filesystem::path &configPath) {
  const auto presetPath =
      std::filesystem::path("/tmp/Music \"wide\" \\ room.effetune_preset");
  const auto saved = pipetune::saveStartupPreset(configPath, presetPath);
  if (!check(saved.empty(), saved)) {
    return false;
  }

  struct stat fileStatus {};
  struct stat directoryStatus {};
  const auto loaded = pipetune::loadStartupPreset(configPath);
  if (!check(loaded.error.empty(), loaded.error) ||
      !check(loaded.found && loaded.presetPath == presetPath,
             "startup preset did not round-trip") ||
      !check(stat(configPath.c_str(), &fileStatus) == 0 &&
                 (fileStatus.st_mode & 0777) == 0600,
             "startup configuration must be private") ||
      !check(stat(configPath.parent_path().c_str(), &directoryStatus) == 0 &&
                 (directoryStatus.st_mode & 0777) == 0700,
             "startup configuration directory must be private")) {
    return false;
  }

  const auto rejected =
      pipetune::saveStartupPreset(configPath, "relative.effetune_preset");
  const auto preserved = pipetune::loadStartupPreset(configPath);
  if (!check(!rejected.empty(), "relative presets must be rejected") ||
      !check(preserved.error.empty() && preserved.found &&
                 preserved.presetPath == presetPath,
             "a rejected save must preserve the previous startup preset")) {
    return false;
  }

  const auto cleared = pipetune::clearStartupPreset(configPath);
  const auto bypass = pipetune::loadStartupPreset(configPath);
  return check(cleared.empty(), cleared) &&
         check(bypass.error.empty() && !bypass.found,
               "cleared configuration must select startup bypass") &&
         check(std::filesystem::exists(configPath),
               "clearing a preset must leave a managed configuration");
}

static bool testFullSnapshotRoundTrip(
    const std::filesystem::path &configPath) {
  const auto expected = configuredSnapshot();
  const auto saved = pipetune::saveStartupConfig(configPath, expected);
  const auto loaded = pipetune::loadStartupConfig(configPath);
  if (!check(saved.empty(), saved) ||
      !check(loaded.error.empty(), loaded.error) ||
      !check(configMatches(loaded.config, expected),
             "complete startup snapshot did not round-trip")) {
    return false;
  }

  auto invalid = expected;
  invalid.presetPath = "relative.effetune_preset";
  const auto rejected = pipetune::saveStartupConfig(configPath, invalid);
  const auto preserved = pipetune::loadStartupConfig(configPath);
  return check(!rejected.empty(),
               "an invalid full snapshot must be rejected") &&
         check(preserved.error.empty(), preserved.error) &&
         check(configMatches(preserved.config, expected),
               "a rejected snapshot must preserve prior settings");
}

static bool testIndependentUpdatesPreserveOtherChoices(
    const std::filesystem::path &configPath) {
  const auto initial = configuredSnapshot();
  if (!check(pipetune::saveStartupConfig(configPath, initial).empty(),
             "cannot seed startup configuration")) {
    return false;
  }

  const auto rate = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 384000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  const auto savedRate = pipetune::saveSampleRatePolicy(configPath, rate);
  const auto afterRate = pipetune::loadStartupConfig(configPath);
  if (!check(savedRate.empty(), savedRate) ||
      !check(afterRate.error.empty(), afterRate.error) ||
      !check(afterRate.config.ratePolicy == rate &&
                 afterRate.config.presetPath == initial.presetPath &&
                 afterRate.config.dspBackend == initial.dspBackend &&
                 afterRate.config.dspSimdVariant == initial.dspSimdVariant,
             "saving a rate must preserve preset and backend choices")) {
    return false;
  }

  const auto savedBackend = pipetune::saveDspBackendSelection(
      configPath, pipetune::DspBackendKind::scalar,
      pipetune::DspSimdVariant::automatic);
  const auto afterBackend = pipetune::loadStartupConfig(configPath);
  if (!check(savedBackend.empty(), savedBackend) ||
      !check(afterBackend.error.empty(), afterBackend.error) ||
      !check(afterBackend.config.dspBackend ==
                     pipetune::DspBackendKind::scalar &&
                 afterBackend.config.dspSimdVariant ==
                     pipetune::DspSimdVariant::automatic &&
                 afterBackend.config.ratePolicy == rate &&
                 afterBackend.config.presetPath == initial.presetPath,
             "saving a backend must preserve preset and rate choices")) {
    return false;
  }

  const auto rejected = pipetune::saveSampleRatePolicy(
      configPath,
      {.mode = pipetune::SampleRateMode::fixed,
       .fixedRate = 88200,
       .enforcement = pipetune::SampleRateEnforcement::suggest});
  const auto preserved = pipetune::loadStartupConfig(configPath);
  return check(!rejected.empty(),
               "an unsupported fixed sample rate must be rejected") &&
         check(preserved.error.empty() &&
                   preserved.config.ratePolicy == rate,
               "a rejected rate must preserve the previous policy");
}

static bool testAcceptedInputForms(
    const std::filesystem::path &configPath) {
  writeConfig(configPath,
              "# Existing package configuration\n"
              "PIPETUNE_PRESET=/tmp/plain.effetune_preset\n"
              "PIPETUNE_DSP_BACKEND=simd\n"
              "PIPETUNE_DSP_SIMD_VARIANT=x86-64-v3\n"
              "PIPETUNE_RATE=96000\n"
              "PIPETUNE_RATE_ENFORCEMENT=force\n");
  const auto unquoted = pipetune::loadStartupConfig(configPath);
  if (!check(unquoted.error.empty(), unquoted.error) ||
      !check(unquoted.config.presetFound &&
                 unquoted.config.presetPath ==
                     "/tmp/plain.effetune_preset" &&
                 unquoted.config.dspBackend ==
                     pipetune::DspBackendKind::simd &&
                 unquoted.config.dspSimdVariant ==
                     pipetune::DspSimdVariant::x86_64_v3 &&
                 unquoted.config.ratePolicy ==
                     pipetune::SampleRatePolicy{
                         .mode = pipetune::SampleRateMode::fixed,
                         .fixedRate = 96000,
                         .enforcement =
                             pipetune::SampleRateEnforcement::force},
             "unquoted assignments must remain readable")) {
    return false;
  }

  writeConfig(configPath,
              "PIPETUNE_PRESET=\"/tmp/quoted \\\"room\\\" "
              "\\\\mix.effetune_preset\"\n");
  const auto quoted = pipetune::loadStartupPreset(configPath);
  if (!check(quoted.error.empty() && quoted.found &&
                 quoted.presetPath ==
                     "/tmp/quoted \"room\" \\mix.effetune_preset",
             "quoted preset escapes must be decoded")) {
    return false;
  }

  writeConfig(configPath, "# Managed by PipeTune.\n");
  const auto defaults = pipetune::loadStartupConfig(configPath);
  std::filesystem::remove(configPath);
  const auto missing = pipetune::loadStartupConfig(configPath);
  const auto expected = pipetune::StartupConfig{};
  return check(defaults.error.empty() &&
                   configMatches(defaults.config, expected),
               "missing assignments must use defaults") &&
         check(missing.error.empty() &&
                   configMatches(missing.config, expected),
               "a missing file must use defaults");
}

static bool testRejectedInputForms(
    const std::filesystem::path &configPath) {
  writeConfig(configPath,
              "PIPETUNE_PRESET=/tmp/one.effetune_preset\n"
              "PIPETUNE_PRESET=/tmp/two.effetune_preset\n");
  const auto duplicatePreset = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath, "PIPETUNE_PRESET=relative.effetune_preset\n");
  const auto relativePreset = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath,
              "PIPETUNE_RATE=48000\nPIPETUNE_RATE=96000\n");
  const auto duplicateRate = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath, "PIPETUNE_RATE=88200\n");
  const auto unsupportedRate = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath, "PIPETUNE_RATE_ENFORCEMENT=strict\n");
  const auto unsupportedEnforcement =
      pipetune::loadStartupConfig(configPath);
  writeConfig(configPath,
              "PIPETUNE_DSP_BACKEND=scalar\n"
              "PIPETUNE_DSP_BACKEND=simd\n");
  const auto duplicateBackend = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath, "PIPETUNE_DSP_BACKEND=avx2\n");
  const auto unsupportedBackend = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath,
              "PIPETUNE_DSP_SIMD_VARIANT=baseline\n"
              "PIPETUNE_DSP_SIMD_VARIANT=sve\n");
  const auto duplicateVariant = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath, "PIPETUNE_DSP_SIMD_VARIANT=avx2\n");
  const auto unsupportedVariant = pipetune::loadStartupConfig(configPath);
  writeConfig(configPath, std::string(64 * 1024 + 1, '#'));
  const auto oversized = pipetune::loadStartupConfig(configPath);

  return check(!duplicatePreset.error.empty(),
               "duplicate preset assignments must be rejected") &&
         check(!relativePreset.error.empty(),
               "relative preset assignments must be rejected") &&
         check(!duplicateRate.error.empty(),
               "duplicate rate assignments must be rejected") &&
         check(!unsupportedRate.error.empty(),
               "unsupported rates must be rejected") &&
         check(!unsupportedEnforcement.error.empty(),
               "unsupported enforcement must be rejected") &&
         check(!duplicateBackend.error.empty(),
               "duplicate backends must be rejected") &&
         check(!unsupportedBackend.error.empty(),
               "unsupported backends must be rejected") &&
         check(!duplicateVariant.error.empty(),
               "duplicate SIMD variants must be rejected") &&
         check(!unsupportedVariant.error.empty(),
               "unsupported SIMD variants must be rejected") &&
         check(!oversized.error.empty(),
               "configurations larger than 64 KiB must be rejected");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-config-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto configPath = directory / "pipetune" / "environment";
  const auto passed =
      testPathResolution() && testPrivatePresetRoundTrip(configPath) &&
      testFullSnapshotRoundTrip(configPath) &&
      testIndependentUpdatesPreserveOtherChoices(configPath) &&
      testAcceptedInputForms(configPath) &&
      testRejectedInputForms(configPath);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
