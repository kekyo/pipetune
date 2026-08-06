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

static bool testPrivateRoundTrip(const std::filesystem::path &configPath) {
  const auto presetPath =
      std::filesystem::path("/tmp/Music \"wide\" \\\\ room.effetune_preset");
  const auto saved = pipetune::saveStartupPreset(configPath, presetPath);
  if (!check(saved.empty(), saved)) {
    return false;
  }

  struct stat fileStatus {};
  struct stat directoryStatus {};
  const auto loaded = pipetune::loadStartupPreset(configPath);
  if (!check(loaded.error.empty(), loaded.error) ||
      !check(loaded.found, "saved startup preset was not found") ||
      !check(loaded.presetPath == presetPath,
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
               "clearing a preset must leave an explicit managed configuration");
}

static bool testFullSnapshotRoundTrip(
    const std::filesystem::path &configPath) {
  const auto expected = pipetune::StartupConfig{
      .presetFound = true,
      .presetPath = "/tmp/snapshot.effetune_preset",
      .preferredOutputFound = true,
      .preferredOutput = "alsa_output.snapshot",
      .ratePolicy =
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 192000,
           .enforcement = pipetune::SampleRateEnforcement::force},
      .dspBackend = pipetune::DspBackendKind::simd,
      .dspSimdVariant = pipetune::DspSimdVariant::x86_64_v3};
  const auto saved = pipetune::saveStartupConfig(configPath, expected);
  const auto loaded = pipetune::loadStartupConfig(configPath);
  if (!check(saved.empty(), saved) ||
      !check(loaded.error.empty(), loaded.error) ||
      !check(loaded.config.presetFound &&
                 loaded.config.presetPath == expected.presetPath,
             "snapshot preset did not round-trip") ||
      !check(loaded.config.preferredOutputFound &&
                 loaded.config.preferredOutput == expected.preferredOutput,
             "snapshot output did not round-trip") ||
      !check(loaded.config.ratePolicy.mode == expected.ratePolicy.mode &&
                 loaded.config.ratePolicy.fixedRate ==
                     expected.ratePolicy.fixedRate &&
                 loaded.config.ratePolicy.enforcement ==
                     expected.ratePolicy.enforcement,
             "snapshot rate policy did not round-trip") ||
      !check(loaded.config.dspBackend == expected.dspBackend &&
                 loaded.config.dspSimdVariant == expected.dspSimdVariant,
             "snapshot DSP choices did not round-trip")) {
    return false;
  }

  auto invalid = expected;
  invalid.preferredOutput.clear();
  const auto rejected = pipetune::saveStartupConfig(configPath, invalid);
  const auto preserved = pipetune::loadStartupConfig(configPath);
  return check(!rejected.empty(),
               "an invalid full snapshot must be rejected") &&
         check(preserved.error.empty(), preserved.error) &&
         check(preserved.config.preferredOutput ==
                   expected.preferredOutput,
               "a rejected snapshot must preserve the prior configuration");
}

static bool testPreferredOutputRoundTripPreservesPreset(
    const std::filesystem::path &configPath) {
  const auto presetPath =
      std::filesystem::path("/tmp/preserved.effetune_preset");
  const auto target = std::string("alsa_output.usb-\"DAC\"\\stereo");
  const auto savedPreset =
      pipetune::saveStartupPreset(configPath, presetPath);
  const auto savedTarget =
      pipetune::savePreferredOutput(configPath, target);
  const auto configured = pipetune::loadStartupConfig(configPath);
  if (!check(savedPreset.empty(), savedPreset) ||
      !check(savedTarget.empty(), savedTarget) ||
      !check(configured.error.empty(), configured.error) ||
      !check(configured.config.presetFound &&
                 configured.config.presetPath == presetPath,
             "saving an output must preserve the startup preset") ||
      !check(configured.config.preferredOutputFound &&
                 configured.config.preferredOutput == target,
             "preferred output did not round-trip")) {
    return false;
  }

  const auto clearedPreset = pipetune::clearStartupPreset(configPath);
  const auto outputOnly = pipetune::loadStartupConfig(configPath);
  if (!check(clearedPreset.empty(), clearedPreset) ||
      !check(outputOnly.error.empty(), outputOnly.error) ||
      !check(!outputOnly.config.presetFound,
             "clearing a preset must remove only its assignment") ||
      !check(outputOnly.config.preferredOutputFound &&
                 outputOnly.config.preferredOutput == target,
             "clearing a preset must preserve the preferred output")) {
    return false;
  }

  const auto restoredPreset =
      pipetune::saveStartupPreset(configPath, presetPath);
  const auto clearedTarget =
      pipetune::clearPreferredOutput(configPath);
  const auto presetOnly = pipetune::loadStartupConfig(configPath);
  return check(restoredPreset.empty(), restoredPreset) &&
         check(clearedTarget.empty(), clearedTarget) &&
         check(presetOnly.error.empty(), presetOnly.error) &&
         check(presetOnly.config.presetFound &&
                   presetOnly.config.presetPath == presetPath,
               "clearing an output must preserve the startup preset") &&
         check(!presetOnly.config.preferredOutputFound &&
                   presetOnly.config.preferredOutput.empty(),
               "clearing an output must remove its assignment");
}

static bool testRatePolicyRoundTripPreservesOtherChoices(
    const std::filesystem::path &configPath) {
  const auto presetPath =
      std::filesystem::path("/tmp/rate-preserved.effetune_preset");
  const auto target = std::string("alsa_output.rate_dac");
  const auto savedPreset =
      pipetune::saveStartupPreset(configPath, presetPath);
  const auto savedTarget =
      pipetune::savePreferredOutput(configPath, target);
  const auto savedRate = pipetune::saveSampleRatePolicy(
      configPath,
      {.mode = pipetune::SampleRateMode::fixed,
       .fixedRate = 384000,
       .enforcement = pipetune::SampleRateEnforcement::force});
  const auto configured = pipetune::loadStartupConfig(configPath);
  if (!check(savedPreset.empty(), savedPreset) ||
      !check(savedTarget.empty(), savedTarget) ||
      !check(savedRate.empty(), savedRate) ||
      !check(configured.error.empty(), configured.error) ||
      !check(configured.config.presetFound &&
                 configured.config.presetPath == presetPath,
             "saving a rate must preserve the startup preset") ||
      !check(configured.config.preferredOutputFound &&
                 configured.config.preferredOutput == target,
             "saving a rate must preserve the preferred output") ||
      !check(configured.config.ratePolicy.mode ==
                     pipetune::SampleRateMode::fixed &&
                 configured.config.ratePolicy.fixedRate == 384000 &&
                 configured.config.ratePolicy.enforcement ==
                     pipetune::SampleRateEnforcement::force,
             "sample-rate policy did not round-trip")) {
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
         check(preserved.error.empty(), preserved.error) &&
         check(preserved.config.ratePolicy.mode ==
                       pipetune::SampleRateMode::fixed &&
                   preserved.config.ratePolicy.fixedRate == 384000 &&
                   preserved.config.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::force,
               "a rejected rate save must preserve the previous policy");
}

static bool testDspBackendRoundTripPreservesOtherChoices(
    const std::filesystem::path &configPath) {
  const auto presetPath =
      std::filesystem::path("/tmp/backend-preserved.effetune_preset");
  const auto savedPreset =
      pipetune::saveStartupPreset(configPath, presetPath);
  const auto savedTarget =
      pipetune::savePreferredOutput(configPath, "alsa_output.backend_dac");
  const auto savedBackend = pipetune::saveDspBackendSelection(
      configPath, pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::x86_64_v3);
  const auto configured = pipetune::loadStartupConfig(configPath);
  if (!check(savedPreset.empty(), savedPreset) ||
      !check(savedTarget.empty(), savedTarget) ||
      !check(savedBackend.empty(), savedBackend) ||
      !check(configured.error.empty(), configured.error) ||
      !check(configured.config.dspBackend ==
                 pipetune::DspBackendKind::simd,
             "DSP backend did not round-trip") ||
      !check(configured.config.dspSimdVariant ==
                 pipetune::DspSimdVariant::x86_64_v3,
             "DSP SIMD variant did not round-trip") ||
      !check(configured.config.presetFound &&
                 configured.config.presetPath == presetPath,
             "saving a DSP backend must preserve the startup preset") ||
      !check(configured.config.preferredOutputFound &&
                 configured.config.preferredOutput ==
                     "alsa_output.backend_dac",
             "saving a DSP backend must preserve the preferred output")) {
    return false;
  }

  const auto savedRate = pipetune::saveSampleRatePolicy(
      configPath,
      {.mode = pipetune::SampleRateMode::fixed,
       .fixedRate = 192000,
       .enforcement = pipetune::SampleRateEnforcement::suggest});
  const auto preserved = pipetune::loadStartupConfig(configPath);
  return check(savedRate.empty(), savedRate) &&
         check(preserved.error.empty(), preserved.error) &&
         check(preserved.config.dspBackend ==
                   pipetune::DspBackendKind::simd,
               "saving another choice must preserve the DSP backend") &&
         check(preserved.config.dspSimdVariant ==
                   pipetune::DspSimdVariant::x86_64_v3,
               "saving another choice must preserve the SIMD variant");
}

static bool testAcceptedInputForms(const std::filesystem::path &configPath) {
  writeConfig(configPath,
              "# Existing package configuration\n"
              "PIPETUNE_PRESET=/tmp/plain.effetune_preset\n"
              "PIPETUNE_TARGET=alsa_output.plain\n"
              "PIPETUNE_DSP_BACKEND=simd\n"
              "PIPETUNE_DSP_SIMD_VARIANT=x86-64-v3\n"
              "PIPETUNE_RATE=96000\n"
              "PIPETUNE_RATE_ENFORCEMENT=force\n");
  const auto unquoted = pipetune::loadStartupConfig(configPath);
  if (!check(unquoted.error.empty() && unquoted.config.presetFound &&
                 unquoted.config.presetPath ==
                     "/tmp/plain.effetune_preset",
             "existing unquoted preset assignments must remain readable") ||
      !check(unquoted.config.preferredOutputFound &&
                 unquoted.config.preferredOutput == "alsa_output.plain",
             "unquoted output assignments must be readable") ||
      !check(unquoted.config.dspBackend ==
                 pipetune::DspBackendKind::simd,
             "unquoted DSP backend assignments must be readable") ||
      !check(unquoted.config.dspSimdVariant ==
                 pipetune::DspSimdVariant::x86_64_v3,
             "unquoted DSP SIMD variant assignments must be readable") ||
      !check(unquoted.config.ratePolicy.mode ==
                     pipetune::SampleRateMode::fixed &&
                 unquoted.config.ratePolicy.fixedRate == 96000 &&
                 unquoted.config.ratePolicy.enforcement ==
                     pipetune::SampleRateEnforcement::force,
             "unquoted rate assignments must be readable")) {
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
  const auto absentConfig = pipetune::loadStartupConfig(configPath);
  const auto absent = pipetune::loadStartupPreset(configPath);
  std::filesystem::remove(configPath);
  const auto missingConfig = pipetune::loadStartupConfig(configPath);
  const auto missing = pipetune::loadStartupPreset(configPath);
  return check(absent.error.empty() && !absent.found,
               "a configuration without PIPETUNE_PRESET must select bypass") &&
         check(missing.error.empty() && !missing.found,
               "a missing configuration must select bypass") &&
         check(absentConfig.error.empty() &&
                   absentConfig.config.dspBackend ==
                       pipetune::DspBackendKind::scalar &&
                   absentConfig.config.dspSimdVariant ==
                       pipetune::DspSimdVariant::automatic &&
                   absentConfig.config.ratePolicy.mode ==
                       pipetune::SampleRateMode::automatic &&
                   absentConfig.config.ratePolicy.fixedRate == 0 &&
                   absentConfig.config.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "missing assignments must default to automatic") &&
         check(missingConfig.error.empty() &&
                   missingConfig.config.dspBackend ==
                       pipetune::DspBackendKind::scalar &&
                   missingConfig.config.dspSimdVariant ==
                       pipetune::DspSimdVariant::automatic &&
                   missingConfig.config.ratePolicy.mode ==
                       pipetune::SampleRateMode::automatic &&
                   missingConfig.config.ratePolicy.fixedRate == 0 &&
                   missingConfig.config.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "a missing file must default to automatic");
}

static bool testRejectedInputForms(const std::filesystem::path &configPath) {
  writeConfig(configPath,
              "PIPETUNE_PRESET=/tmp/one.effetune_preset\n"
              "PIPETUNE_PRESET=/tmp/two.effetune_preset\n");
  const auto duplicate = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath,
              "PIPETUNE_PRESET=relative.effetune_preset\n");
  const auto relative = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath,
              "PIPETUNE_PRESET=\"/tmp/broken\\q.effetune_preset\"\n");
  const auto malformed = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath,
              "PIPETUNE_TARGET=alsa_output.one\n"
              "PIPETUNE_TARGET=alsa_output.two\n");
  const auto duplicateTarget = pipetune::loadStartupConfig(configPath);

  writeConfig(configPath,
              "PIPETUNE_RATE=48000\n"
              "PIPETUNE_RATE=96000\n"
              "PIPETUNE_RATE_ENFORCEMENT=suggest\n");
  const auto duplicateRate = pipetune::loadStartupConfig(configPath);

  writeConfig(configPath,
              "PIPETUNE_RATE=88200\n"
              "PIPETUNE_RATE_ENFORCEMENT=suggest\n");
  const auto unsupportedRate = pipetune::loadStartupConfig(configPath);

  writeConfig(configPath,
              "PIPETUNE_RATE=automatic\n"
              "PIPETUNE_RATE_ENFORCEMENT=strict\n");
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

  writeConfig(configPath,
              std::string(64 * 1024 + 1, '#'));
  const auto oversized = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath, "# Managed by PipeTune.\n");
  const auto rejectedEmptyTarget =
      pipetune::savePreferredOutput(configPath, "");

  return check(!duplicate.error.empty(),
               "duplicate preset assignments must be rejected") &&
         check(!relative.error.empty(),
               "relative preset assignments must be rejected") &&
         check(!malformed.error.empty(),
               "malformed quoted assignments must be rejected") &&
         check(!duplicateTarget.error.empty(),
               "duplicate output assignments must be rejected") &&
         check(!duplicateRate.error.empty(),
               "duplicate rate assignments must be rejected") &&
         check(!unsupportedRate.error.empty(),
               "unsupported configured rates must be rejected") &&
         check(!unsupportedEnforcement.error.empty(),
               "unsupported enforcement must be rejected") &&
         check(!duplicateBackend.error.empty(),
               "duplicate DSP backend assignments must be rejected") &&
         check(!unsupportedBackend.error.empty(),
               "unsupported DSP backends must be rejected") &&
         check(!duplicateVariant.error.empty(),
               "duplicate DSP SIMD variants must be rejected") &&
         check(!unsupportedVariant.error.empty(),
               "unsupported DSP SIMD variants must be rejected") &&
         check(!oversized.error.empty(),
               "startup configurations larger than 64 KiB must be rejected") &&
         check(!rejectedEmptyTarget.empty(),
               "empty preferred output names must be rejected");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-config-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto configPath = directory / "pipetune" / "environment";
  const auto passed =
      testPathResolution() && testPrivateRoundTrip(configPath) &&
      testFullSnapshotRoundTrip(configPath) &&
      testPreferredOutputRoundTripPreservesPreset(configPath) &&
      testRatePolicyRoundTripPreservesOtherChoices(configPath) &&
      testDspBackendRoundTripPreservesOtherChoices(configPath) &&
      testAcceptedInputForms(configPath) && testRejectedInputForms(configPath);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
