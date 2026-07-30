#include "dsp_backend_runtime.h"
#include "dsp_pipeline_slot.h"

#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_pipeline.h"

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

static pipetune::DspSimdVariant pinForVariant(
    pipetune::DspBackendVariant variant) {
  switch (variant) {
  case pipetune::DspBackendVariant::simdBaseline:
    return pipetune::DspSimdVariant::baseline;
  case pipetune::DspBackendVariant::x86_64_v3:
    return pipetune::DspSimdVariant::x86_64_v3;
  case pipetune::DspBackendVariant::x86_64_v4:
    return pipetune::DspSimdVariant::x86_64_v4;
  case pipetune::DspBackendVariant::arm64Sve:
    return pipetune::DspSimdVariant::arm64Sve;
  case pipetune::DspBackendVariant::scalar:
    break;
  }
  return pipetune::DspSimdVariant::automatic;
}

static std::filesystem::path
writePreset(const std::filesystem::path &directory) {
  const auto path = directory / "backend-runtime.effetune_preset";
  auto stream = std::ofstream(path, std::ios::binary);
  stream << R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}]})json";
  return path;
}

static bool testLiveSwitch(const std::filesystem::path &presetPath,
                           const pipetune::DspBackends &backends) {
  auto loaded = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      backends.scalar.backend);
  if (!check(loaded.pipeline != nullptr, loaded.error)) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(loaded.pipeline));
  auto state = pipetune::makeDspBackendRuntimeState(
      backends, pipetune::DspBackendKind::scalar,
      pipetune::DspSimdVariant::automatic);
  auto samples = std::vector<float>{0.5F};
  if (!check(slot.process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "scalar processing failed before live backend switch")) {
    return false;
  }
  const auto counters = slot.performanceCounters();
  const auto switched = pipetune::switchDspBackend(
      slot, state, pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::automatic,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      false);
  samples[0] = 0.5F;
  if (!check(switched.error.empty(), switched.error) ||
      !check(switched.changed, "backend switch must report a state change") ||
      !check(state.configuredBackend ==
                     pipetune::DspBackendKind::simd &&
                 state.effectiveBackend ==
                     pipetune::DspBackendKind::simd &&
                 state.configuredSimdVariant ==
                     pipetune::DspSimdVariant::automatic &&
                 state.effectiveVariant ==
                     backends.simd.backend->variant() &&
                 !state.fallback && state.error.empty(),
             "successful backend switch state differs") ||
      !check(slot.backendKind() == pipetune::DspBackendKind::simd,
             "successful backend switch must replace the pipeline") ||
      !check(slot.process(samples, 1, 1, 0.1) ==
                 pipetune::ProcessStatus::ok,
             "SIMD processing failed after live backend switch") ||
      !check(slot.performanceCounters().processedFrames ==
                 counters.processedFrames + 1,
             "live backend switch must preserve cumulative DSP counters")) {
    return false;
  }

  const auto effectiveVariant = *state.effectiveVariant;
  const auto pinnedPreference = pinForVariant(effectiveVariant);
  const auto pinned = pipetune::switchDspBackend(
      slot, state, pipetune::DspBackendKind::simd, pinnedPreference,
      {.sampleRate = 0.0F, .maxChannels = 1, .maxFrames = 32}, false);
  if (!check(pinned.error.empty(), pinned.error) ||
      !check(pinned.changed,
             "pinning the automatic effective tier must change selection") ||
      !check(state.configuredSimdVariant == pinnedPreference &&
                 state.effectiveVariant == effectiveVariant &&
                 slot.backendVariant() == effectiveVariant,
             "same-tier pinning must preserve the effective DSP backend")) {
    return false;
  }

  const auto failedRebuild = pipetune::switchDspBackend(
      slot, state, pipetune::DspBackendKind::scalar,
      {.sampleRate = 0.0F, .maxChannels = 1, .maxFrames = 32},
      false);
  if (!check(!failedRebuild.error.empty(),
             "failed backend rebuild must report a diagnostic") ||
      !check(!failedRebuild.changed,
             "failed backend rebuild must not report a change") ||
      !check(state.configuredBackend ==
                     pipetune::DspBackendKind::simd &&
                 state.effectiveBackend ==
                     pipetune::DspBackendKind::simd &&
                 slot.backendKind() ==
                     pipetune::DspBackendKind::simd,
             "failed backend rebuild must preserve the old state")) {
    return false;
  }

  const auto rejected = pipetune::switchDspBackend(
      slot, state, pipetune::DspBackendKind::scalar,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      true);
  return check(!rejected.error.empty(),
               "backend switch during rate transition must be rejected") &&
         check(!rejected.changed,
               "rejected backend switch must not report a change") &&
         check(state.configuredBackend ==
                       pipetune::DspBackendKind::simd &&
                   slot.backendKind() ==
                       pipetune::DspBackendKind::simd,
               "rejected backend switch must preserve the old state");
}

static bool testUnavailableSimdPreservesState(
    const std::filesystem::path &presetPath,
    const pipetune::DspBackends &discovered) {
  auto backends = discovered;
  backends.simd.backend.reset();
  backends.simd.error = "test SIMD backend is unavailable";
  for (auto &variant : backends.simdVariants) {
    variant.backend.reset();
    variant.cpuSupported = true;
    variant.error = "test SIMD backend is unavailable";
  }
  auto loaded = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      backends.scalar.backend);
  if (!check(loaded.pipeline != nullptr, loaded.error)) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(loaded.pipeline));
  auto state = pipetune::makeDspBackendRuntimeState(
      backends, pipetune::DspBackendKind::scalar,
      pipetune::DspSimdVariant::automatic);
  const auto rejected = pipetune::switchDspBackend(
      slot, state, pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::arm64Sve,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      false);
  if (!check(!rejected.error.empty(),
             "unavailable pinned SIMD live switch must fail") ||
      !check(!rejected.changed,
             "unavailable pinned SIMD must not change runtime state") ||
      !check(state.configuredBackend ==
                     pipetune::DspBackendKind::scalar &&
                 state.effectiveBackend ==
                     pipetune::DspBackendKind::scalar &&
                 slot.backendKind() ==
                     pipetune::DspBackendKind::scalar,
             "unavailable SIMD request must preserve scalar state")) {
    return false;
  }

  const auto startupFallback = pipetune::makeDspBackendRuntimeState(
      backends, pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::automatic);
  return check(startupFallback.configuredBackend ==
                       pipetune::DspBackendKind::simd &&
                   startupFallback.effectiveBackend ==
                       pipetune::DspBackendKind::scalar &&
                   startupFallback.fallback &&
                   startupFallback.error ==
                       "test SIMD backend is unavailable",
               "startup SIMD fallback runtime state differs");
}

int main() {
  const auto backends = pipetune::discoverDspBackends();
  if (!check(backends.scalar.backend != nullptr, backends.scalar.error) ||
      !check(backends.simd.backend != nullptr, backends.simd.error)) {
    return 1;
  }
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-backend-runtime-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto preset = writePreset(directory);
  const auto passed = testLiveSwitch(preset, backends) &&
                      testUnavailableSimdPreservesState(preset, backends);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
