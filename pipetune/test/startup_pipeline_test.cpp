#include "startup_pipeline.h"

#include "pipetune/dsp_backend.h"
#include "pipetune/startup_config.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testIntentionalBypass(const std::filesystem::path &configPath) {
  const auto savedRate = pipetune::saveSampleRatePolicy(
      configPath,
      {.mode = pipetune::SampleRateMode::fixed,
       .fixedRate = 384000,
       .enforcement = pipetune::SampleRateEnforcement::force});
  if (!check(savedRate.empty(), savedRate)) {
    return false;
  }
  const auto prepared = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(prepared.pipeline != nullptr, prepared.configurationError) ||
      !check(prepared.activePresetPath.empty(),
             "missing startup configuration must not select a preset") ||
      !check(prepared.configurationError.empty(),
             "missing startup configuration must be intentional bypass") ||
      !check(prepared.ratePolicy ==
                 pipetune::SampleRatePolicy{
                     .mode = pipetune::SampleRateMode::fixed,
                     .fixedRate = 384000,
                     .enforcement =
                         pipetune::SampleRateEnforcement::force},
             "startup rate policy must be returned in bypass mode") ||
      !check(prepared.pipeline->sampleRate() == 384000.0F,
             "fixed-rate startup bypass must be prepared at the configured "
             "DSP rate") ||
      !check(prepared.pipeline->activePluginCount() == 0,
             "startup bypass must not contain DSP nodes")) {
    return false;
  }
  auto samples = std::vector<float>{0.5F, -0.25F, -0.5F, 0.25F};
  const auto original = samples;
  return check(prepared.pipeline->process(samples, 2, 2, 0.0) ==
                   pipetune::ProcessStatus::ok,
               "startup bypass processing failed") &&
         check(samples == original,
               "startup bypass must leave PCM unchanged");
}

static bool testConfiguredPreset(const std::filesystem::path &configPath,
                                 const std::filesystem::path &presetPath) {
  {
    auto preset = std::ofstream(presetPath, std::ios::binary);
    preset << R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}]})json";
  }
  const auto saved = pipetune::saveStartupPreset(configPath, presetPath);
  const auto savedBackend = pipetune::saveDspBackendKind(
      configPath, pipetune::DspBackendKind::simd);
  if (!check(saved.empty(), saved) ||
      !check(savedBackend.empty(), savedBackend)) {
    return false;
  }
  const auto prepared = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(prepared.pipeline != nullptr, prepared.configurationError) ||
      !check(prepared.activePresetPath == presetPath,
             "configured preset path differs") ||
      !check(prepared.configurationError.empty(),
             "valid configured preset must not report an error") ||
      !check(prepared.ratePolicy ==
                 pipetune::SampleRatePolicy{
                     .mode = pipetune::SampleRateMode::fixed,
                     .fixedRate = 384000,
                     .enforcement =
                         pipetune::SampleRateEnforcement::force},
             "loading a preset must preserve the startup rate policy") ||
      !check(prepared.pipeline->sampleRate() == 384000.0F,
             "fixed-rate startup preset must be prepared at the configured "
             "DSP rate") ||
      !check(prepared.pipeline->activePluginCount() == 1,
             "configured preset must prepare its DSP node") ||
      !check(prepared.configuredDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 prepared.effectiveDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 !prepared.dspBackendFallback &&
                 prepared.dspBackendError.empty(),
             "available configured SIMD backend must become effective") ||
      !check(prepared.pipeline->backendKind() ==
                 pipetune::DspBackendKind::simd,
             "configured preset must use the selected SIMD backend")) {
    return false;
  }
  auto samples = std::vector<float>{1.0F, -1.0F};
  if (!check(prepared.pipeline->process(samples, 2, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "configured preset processing failed")) {
    return false;
  }
  const auto gain = std::pow(10.0F, -6.0F / 20.0F);
  return check(std::abs(samples[0] - gain) < 1.0e-6F &&
                   std::abs(samples[1] + gain) < 1.0e-6F,
               "configured preset must process PCM");
}

static bool testConfiguredSimdFallback(
    const std::filesystem::path &configPath,
    const std::filesystem::path &presetPath) {
  auto backends = pipetune::discoverDspBackends();
  if (!check(backends.scalar.backend != nullptr, backends.scalar.error) ||
      !check(backends.simd.backend != nullptr, backends.simd.error)) {
    return false;
  }
  backends.simd.backend.reset();
  backends.simd.error = "test SIMD backend is unavailable";
  for (auto &variant : backends.simdVariants) {
    variant.backend.reset();
    variant.error = "test SIMD backend is unavailable";
  }

  const auto prepared = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64},
      backends);
  return check(prepared.pipeline != nullptr, prepared.error) &&
         check(prepared.activePresetPath == presetPath,
               "SIMD fallback must retain the configured preset") &&
         check(prepared.configuredDspBackend ==
                   pipetune::DspBackendKind::simd,
               "SIMD fallback must retain the configured backend") &&
         check(prepared.effectiveDspBackend ==
                   pipetune::DspBackendKind::scalar,
               "unavailable SIMD must use scalar") &&
         check(prepared.dspBackendFallback,
               "unavailable SIMD must report fallback") &&
         check(prepared.dspBackendError.find(
                   "test SIMD backend is unavailable") !=
                   std::string::npos,
               "SIMD fallback must report its cause") &&
         check(prepared.pipeline->backendKind() ==
                   pipetune::DspBackendKind::scalar,
               "fallback preset must be built by scalar DSP");
}

static bool testDegradedBypass(const std::filesystem::path &configPath,
                               const std::filesystem::path &missingPreset) {
  const auto saved =
      pipetune::saveStartupPreset(configPath, missingPreset);
  if (!check(saved.empty(), saved)) {
    return false;
  }
  const auto missing = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(missing.pipeline != nullptr,
             "a missing configured preset must still prepare bypass") ||
      !check(missing.activePresetPath.empty(),
             "degraded bypass must not report an active preset") ||
      !check(!missing.configurationError.empty(),
             "a missing configured preset must report its diagnostic") ||
      !check(missing.pipeline->activePluginCount() == 0,
             "degraded bypass must not contain DSP nodes")) {
    return false;
  }

  {
    auto config = std::ofstream(configPath, std::ios::binary);
    config << "PIPETUNE_PRESET=relative.effetune_preset\n";
  }
  const auto malformed = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  return check(malformed.pipeline != nullptr,
               "an invalid startup configuration must still prepare bypass") &&
         check(malformed.activePresetPath.empty(),
               "invalid configuration bypass must not report a preset") &&
         check(!malformed.configurationError.empty(),
               "an invalid startup configuration must report its diagnostic");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-startup-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "environment";
  const auto presetPath = directory / "configured.effetune_preset";
  const auto missingPreset = directory / "missing.effetune_preset";
  const auto passed =
      testIntentionalBypass(configPath) &&
      testConfiguredPreset(configPath, presetPath) &&
      testConfiguredSimdFallback(configPath, presetPath) &&
      testDegradedBypass(configPath, missingPreset);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
