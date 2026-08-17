/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_pipeline_slot.h"
#include "dsp_idle_runtime_state.h"

#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

#if defined(__SSE__) || defined(_M_X64)
#include <immintrin.h>
#endif

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static std::filesystem::path writeVolumePreset(
    const std::filesystem::path &directory, std::string_view name,
    int decibels) {
  const auto path = directory / name;
  auto stream = std::ofstream(path, std::ios::binary);
  stream << R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{"vl":)json"
         << decibels << R"json(},"channel":"A"}]})json";
  return path;
}

static std::filesystem::path writeDcOffsetPreset(
    const std::filesystem::path &directory) {
  const auto path = directory / "dc-offset.effetune_preset";
  auto stream = std::ofstream(path, std::ios::binary);
  stream << R"json({"pipeline":[{"name":"DC Offset","enabled":true,"parameters":{"of":0.5},"channel":"A"}]})json";
  return path;
}

static std::filesystem::path writeDelayPreset(
    const std::filesystem::path &directory) {
  const auto path = directory / "delay.effetune_preset";
  auto stream = std::ofstream(path, std::ios::binary);
  stream << R"json({"pipeline":[{"name":"Delay","enabled":true,"parameters":{"pd":0,"ds":1,"dp":0,"hd":20000,"ld":20,"mx":100,"fb":0,"pp":0},"channel":"A"}]})json";
  return path;
}

static std::unique_ptr<pipetune::DspPipeline> loadPipeline(
    const std::filesystem::path &path) {
  auto loaded = pipetune::loadDspPipeline(
      path, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 32});
  if (loaded.pipeline == nullptr) {
    std::cerr << loaded.error << '\n';
  }
  return std::move(loaded.pipeline);
}

static bool approximately(float actual, float expected) {
  return std::abs(actual - expected) <= 1.0e-6F;
}

static bool testReplacementChangesPcm(
    const std::filesystem::path &positivePath,
    const std::filesystem::path &negativePath) {
  auto initial = loadPipeline(positivePath);
  auto replacement = loadPipeline(negativePath);
  if (initial == nullptr || replacement == nullptr) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(initial));
  auto samples = std::vector<float>{0.25F};
  const auto initialResult =
      slot.processWithGeneration(samples, 1, 1, 0.0);
  if (!check(initialResult.status == pipetune::ProcessStatus::ok,
             "initial slot processing failed") ||
      !check(initialResult.generation == 0,
             "initial pipeline generation must be zero") ||
      !check(approximately(samples[0],
                           0.25F * std::pow(10.0F, 6.0F / 20.0F)),
             "initial slot PCM differs") ||
      !check(slot.performanceCounters().processedFrames == 1,
             "initial DSP frame count differs") ||
      !check(slot.performanceCounters().processingNanoseconds > 0,
             "initial DSP processing time was not measured")) {
    return false;
  }

  const auto initialCounters = slot.performanceCounters();
  slot.replace(std::move(replacement));
  samples[0] = 0.25F;
  const auto replacementResult =
      slot.processWithGeneration(samples, 1, 1, 0.1);
  return check(replacementResult.status == pipetune::ProcessStatus::ok,
               "replacement slot processing failed") &&
         check(replacementResult.generation == 1,
               "replacement pipeline generation must advance") &&
         check(approximately(samples[0],
                             0.25F * std::pow(10.0F, -6.0F / 20.0F)),
               "replacement slot PCM differs") &&
         check(slot.activePluginCount() == 1,
               "slot must report replacement plugin count") &&
         check(slot.performanceCounters().processedFrames == 2,
               "DSP frame count must survive pipeline replacement") &&
         check(slot.performanceCounters().processingNanoseconds >=
                   initialCounters.processingNanoseconds,
               "DSP processing time must survive pipeline replacement");
}

static bool testBypassDoesNotReportDspWork() {
  auto created = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(created.pipeline != nullptr, created.error)) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(created.pipeline));
  auto samples = std::vector<float>(32, 0.25F);
  const auto result = slot.processWithIdle(
      samples, 1, 32, 0.0, {.timeoutMilliseconds = 100});
  if (!check(result.status == pipetune::ProcessStatus::ok,
             "bypass slot processing failed") ||
      !check(result.activity == pipetune::DspActivity::bypassed,
             "bypass slot must report bypassed activity")) {
    return false;
  }
  const auto counters = slot.performanceCounters();
  return check(counters.processedFrames == 0 &&
                   counters.processingNanoseconds == 0,
               "bypass must not be counted as EffeTune DSP work");
}

static bool allApproximately(std::span<const float> samples,
                             float expected) {
  for (const auto sample : samples) {
    if (!approximately(sample, expected)) {
      return false;
    }
  }
  return true;
}

static bool testIdleSuspensionFadesGeneratedOutputAndStopsDsp(
    const std::filesystem::path &presetPath) {
  auto pipeline = loadPipeline(presetPath);
  if (pipeline == nullptr) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(pipeline));
  constexpr auto policy =
      pipetune::DspIdlePolicy{.timeoutMilliseconds = 100};
  auto samples = std::vector<float>(32, 0.0F);
  auto result = pipetune::DspIdleProcessResult{};

  for (auto block = std::uint32_t{0}; block < 150; ++block) {
    std::fill(samples.begin(), samples.end(), 0.0F);
    result = slot.processWithIdle(
        samples, 1, 32, static_cast<double>(block * 32) / 48000.0,
        policy);
    if (!check(result.status == pipetune::ProcessStatus::ok,
               "DSP failed during the silence allowance") ||
        !check(result.activity == pipetune::DspActivity::draining,
               "silent DSP must drain before the timeout")) {
      return false;
    }
  }
  if (!check(allApproximately(samples, 0.5F),
             "source-generating DSP must remain audible through the timeout") ||
      !check(slot.performanceCounters().processedFrames == 4800,
             "DSP work before the timeout differs")) {
    return false;
  }

  std::fill(samples.begin(), samples.end(), 0.0F);
  result = slot.processWithIdle(samples, 1, 32, 0.1, policy);
  if (!check(result.status == pipetune::ProcessStatus::ok &&
                 result.activity == pipetune::DspActivity::draining,
             "DSP must drain while applying the suspension fade") ||
      !check(approximately(samples.front(), 0.5F) &&
                 samples.back() > 0.0F &&
                 samples.back() < samples.front(),
             "DSP suspension fade did not attenuate generated output")) {
    return false;
  }

  for (auto block = std::uint32_t{0}; block < 7; ++block) {
    std::fill(samples.begin(), samples.end(), 0.0F);
    result = slot.processWithIdle(
        samples, 1, 32,
        0.1 + static_cast<double>((block + 1) * 32) / 48000.0,
        policy);
  }
  if (!check(result.status == pipetune::ProcessStatus::ok &&
                 result.activity == pipetune::DspActivity::sleeping,
             "DSP did not suspend after the fade") ||
      !check(approximately(samples.back(), 0.0F),
             "suspension fade must end at exact silence")) {
    return false;
  }

  const auto sleepingCounters = slot.performanceCounters();
  std::fill(samples.begin(), samples.end(), -0.0F);
  result = slot.processWithIdle(samples, 1, 32, 0.2, policy);
  if (!check(result.status == pipetune::ProcessStatus::ok &&
                 result.activity == pipetune::DspActivity::sleeping,
             "signed zero must keep DSP suspended") ||
      !check(slot.performanceCounters().processedFrames ==
                 sleepingCounters.processedFrames,
             "suspended silence must not invoke native DSP") ||
      !check(allApproximately(samples, 0.0F),
             "suspended DSP must produce exact silence")) {
    return false;
  }

  samples.resize(64);
  std::fill(samples.begin(), samples.end(), 0.0F);
  samples[32] = std::numeric_limits<float>::denorm_min();
  result = slot.processWithIdle(samples, 2, 32, 0.3, policy);
  return check(result.status == pipetune::ProcessStatus::ok &&
                   result.activity == pipetune::DspActivity::active,
               "non-zero subnormal input must wake DSP") &&
         check(slot.performanceCounters().processedFrames ==
                   sleepingCounters.processedFrames + 32,
               "the first non-zero block must be processed after wake") &&
         check(allApproximately(samples, 0.5F),
               "the waking block must not be discarded");
}

static bool testStaleDspActivityCannotOverwriteControlState() {
  auto state = pipetune::DspIdleRuntimeState(
      {.timeoutMilliseconds = 100}, pipetune::DspActivity::active);
  const auto processingSnapshot = state.load();
  const auto policyChanged = state.replacePolicy(
      {}, pipetune::DspActivity::active);
  if (!check(policyChanged,
             "DSP idle runtime policy change was not reported") ||
      !check(!state.tryReplaceActivity(
                  processingSnapshot, pipetune::DspActivity::sleeping),
             "stale DSP callback overwrote a newer policy")) {
    return false;
  }
  const auto ignored = state.load();
  if (!check(!pipetune::dspIdlePolicyIsEnabled(ignored.policy) &&
                 ignored.activity == pipetune::DspActivity::active,
             "policy replacement did not remain internally consistent")) {
    return false;
  }

  const auto beforeBypass = state.load();
  state.replaceActivity(pipetune::DspActivity::bypassed);
  return check(!state.tryReplaceActivity(
                   beforeBypass, pipetune::DspActivity::draining),
               "stale DSP callback overwrote explicit bypass") &&
         check(state.load().activity == pipetune::DspActivity::bypassed,
               "explicit bypass activity was not retained");
}

static bool testIgnoredIdlePolicyKeepsDspActive(
    const std::filesystem::path &presetPath) {
  auto pipeline = loadPipeline(presetPath);
  if (pipeline == nullptr) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(pipeline));
  auto samples = std::vector<float>(32, 0.0F);
  const auto result = slot.processWithIdle(samples, 1, 32, 0.0, {});
  return check(result.status == pipetune::ProcessStatus::ok &&
                   result.activity == pipetune::DspActivity::active,
               "ignored idle policy must keep DSP active") &&
         check(slot.performanceCounters().processedFrames == 32,
               "ignored idle policy must invoke native DSP") &&
         check(allApproximately(samples, 0.5F),
               "ignored idle policy must preserve generated output");
}

static bool testEngineResetClearsDelayState(
    const std::filesystem::path &presetPath) {
  auto reference = loadPipeline(presetPath);
  auto resetPipeline = loadPipeline(presetPath);
  if (reference == nullptr || resetPipeline == nullptr) {
    return false;
  }
  auto referenceSamples = std::vector<float>(32, 0.0F);
  auto resetSamples = std::vector<float>(32, 0.0F);
  referenceSamples[0] = 1.0F;
  resetSamples[0] = 1.0F;
  if (!check(reference->process(referenceSamples, 1, 32, 0.0) ==
                 pipetune::ProcessStatus::ok &&
                 resetPipeline->process(resetSamples, 1, 32, 0.0) ==
                     pipetune::ProcessStatus::ok,
             "delay setup processing failed") ||
      !check(resetPipeline->reset() == pipetune::ProcessStatus::ok,
             "native engine reset failed")) {
    return false;
  }
  std::fill(referenceSamples.begin(), referenceSamples.end(), 0.0F);
  std::fill(resetSamples.begin(), resetSamples.end(), 0.0F);
  if (!check(reference->process(referenceSamples, 1, 32, 32.0 / 48000.0) ==
                 pipetune::ProcessStatus::ok &&
                 resetPipeline->process(resetSamples, 1, 32,
                                        32.0 / 48000.0) ==
                     pipetune::ProcessStatus::ok,
             "delay verification processing failed")) {
    return false;
  }
  const auto referencePeak = *std::max_element(referenceSamples.begin(),
                                                referenceSamples.end());
  return check(referencePeak > 0.9F,
               "delay reference did not retain its impulse") &&
         check(allApproximately(resetSamples, 0.0F),
               "engine reset must clear retained delay audio");
}

static bool testNativeProcessingEnablesDenormalFlush(
    const std::filesystem::path &presetPath) {
#if defined(__SSE__) || defined(_M_X64)
  auto pipeline = loadPipeline(presetPath);
  if (pipeline == nullptr) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(pipeline));
  auto samples = std::vector<float>{0.25F};
  constexpr auto denormalModeMask = static_cast<unsigned int>(
      _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK);
  const auto originalControl = _mm_getcsr();
  const auto disabledControl = originalControl & ~denormalModeMask;
  _mm_setcsr(disabledControl);
  const auto status = slot.process(samples, 1, 1, 0.0);
  const auto configuredControl = _mm_getcsr();
  _mm_setcsr(originalControl);
  constexpr auto exceptionStatusMask =
      static_cast<unsigned int>(_MM_EXCEPT_MASK);

  return check(status == pipetune::ProcessStatus::ok,
               "native processing failed while configuring denormal mode") &&
         check((configuredControl & denormalModeMask) == denormalModeMask,
               "native processing must enable FTZ and DAZ") &&
         check((configuredControl &
                ~(denormalModeMask | exceptionStatusMask)) ==
                   (disabledControl &
                    ~(denormalModeMask | exceptionStatusMask)),
               "native processing must preserve unrelated MXCSR state");
#else
  static_cast<void>(presetPath);
  return true;
#endif
}

static bool testConcurrentReplacementProducesOnlyCompletePipelines(
    const std::filesystem::path &positivePath,
    const std::filesystem::path &negativePath) {
  auto initial = loadPipeline(positivePath);
  auto replacement = loadPipeline(negativePath);
  if (initial == nullptr || replacement == nullptr) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(initial));
  auto started = std::atomic<bool>{false};
  auto replaced = std::atomic<bool>{false};
  auto observedReplacement = std::atomic<bool>{false};
  auto failed = std::atomic<bool>{false};
  const auto positive = 0.25F * std::pow(10.0F, 6.0F / 20.0F);
  const auto negative = 0.25F * std::pow(10.0F, -6.0F / 20.0F);

  auto processor = std::thread([&] {
    started.store(true, std::memory_order_release);
    for (auto iteration = std::uint32_t{0}; iteration < 1000000;
         ++iteration) {
      auto samples = std::vector<float>{0.25F};
      const auto result = slot.processWithGeneration(
          samples, 1, 1,
          static_cast<double>(iteration) / 48000.0);
      const auto generationMatches =
          (result.generation == 0 &&
           approximately(samples[0], positive)) ||
          (result.generation == 1 &&
           approximately(samples[0], negative));
      if (result.status != pipetune::ProcessStatus::ok ||
          !generationMatches) {
        failed.store(true, std::memory_order_relaxed);
        break;
      }
      if (replaced.load(std::memory_order_acquire) &&
          approximately(samples[0], negative)) {
        observedReplacement.store(true, std::memory_order_release);
        break;
      }
    }
  });

  while (!started.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  slot.replace(std::move(replacement));
  replaced.store(true, std::memory_order_release);
  processor.join();
  return check(!failed.load(std::memory_order_relaxed),
               "concurrent replacement produced invalid PCM") &&
         check(observedReplacement.load(std::memory_order_acquire),
               "processor did not observe the replacement pipeline");
}

static bool testStagedReplacementCanCommitAndRollback(
    const std::filesystem::path &positivePath,
    const std::filesystem::path &negativePath) {
  auto initial = loadPipeline(positivePath);
  auto replacement = loadPipeline(negativePath);
  if (initial == nullptr || replacement == nullptr) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(initial));
  slot.stageReplacement(std::move(replacement));
  auto samples = std::vector<float>{0.25F};
  if (!check(slot.hasStagedReplacement(),
             "staged replacement must retain rollback state") ||
      !check(slot.process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok &&
                 approximately(samples[0],
                               0.25F * std::pow(10.0F, -6.0F / 20.0F)),
             "staged replacement must become active")) {
    return false;
  }

  slot.rollbackStaged();
  samples[0] = 0.25F;
  if (!check(!slot.hasStagedReplacement(),
             "rollback must close the transaction") ||
      !check(slot.process(samples, 1, 1, 0.1) ==
                 pipetune::ProcessStatus::ok &&
                 approximately(samples[0],
                               0.25F * std::pow(10.0F, 6.0F / 20.0F)),
             "rollback must restore the exact previous pipeline")) {
    return false;
  }

  auto rebuilt = slot.rebuildActive(
      {.sampleRate = 96000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(rebuilt.pipeline != nullptr, rebuilt.error) ||
      !check(rebuilt.pipeline->sampleRate() == 96000.0F,
             "slot rebuild must use its retained recipe")) {
    return false;
  }
  slot.stageReplacement(std::move(rebuilt.pipeline));
  slot.commitStaged();
  return check(!slot.hasStagedReplacement(),
               "commit must release rollback state");
}

static bool testBackendReplacementPreservesCounters(
    const std::filesystem::path &presetPath) {
  const auto backends = pipetune::discoverDspBackends();
  if (!check(backends.scalar.backend != nullptr, backends.scalar.error) ||
      !check(backends.simd.backend != nullptr, backends.simd.error)) {
    return false;
  }
  auto initial = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      backends.scalar.backend);
  if (!check(initial.pipeline != nullptr, initial.error)) {
    return false;
  }
  auto slot = pipetune::DspPipelineSlot(std::move(initial.pipeline));
  auto samples = std::vector<float>{0.25F};
  if (!check(slot.process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "scalar pipeline processing failed before backend switch")) {
    return false;
  }
  const auto before = slot.performanceCounters();
  auto replacement = slot.rebuildActive(
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32},
      backends.simd.backend);
  if (!check(replacement.pipeline != nullptr, replacement.error) ||
      !check(replacement.pipeline->backendKind() ==
                 pipetune::DspBackendKind::simd,
             "backend rebuild must use SIMD")) {
    return false;
  }
  slot.replace(std::move(replacement.pipeline));
  samples[0] = 0.25F;
  return check(slot.backendKind() == pipetune::DspBackendKind::simd,
               "slot must expose its effective replacement backend") &&
         check(slot.process(samples, 1, 1, 0.1) ==
                   pipetune::ProcessStatus::ok,
               "SIMD pipeline processing failed after backend switch") &&
         check(slot.performanceCounters().processedFrames ==
                   before.processedFrames + 1,
               "backend switch must preserve cumulative DSP frames") &&
         check(slot.performanceCounters().processingNanoseconds >=
                   before.processingNanoseconds,
               "backend switch must preserve cumulative DSP time");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-slot-test-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto positive =
      writeVolumePreset(directory, "positive.effetune_preset", 6);
  const auto negative =
      writeVolumePreset(directory, "negative.effetune_preset", -6);
  const auto dcOffset = writeDcOffsetPreset(directory);
  const auto delay = writeDelayPreset(directory);
  const auto passed = testReplacementChangesPcm(positive, negative) &&
                      testBypassDoesNotReportDspWork() &&
                      testIdleSuspensionFadesGeneratedOutputAndStopsDsp(
                          dcOffset) &&
                      testStaleDspActivityCannotOverwriteControlState() &&
                      testIgnoredIdlePolicyKeepsDspActive(dcOffset) &&
                      testEngineResetClearsDelayState(delay) &&
                      testNativeProcessingEnablesDenormalFlush(positive) &&
                      testConcurrentReplacementProducesOnlyCompletePipelines(
                          positive, negative) &&
                      testStagedReplacementCanCommitAndRollback(
                          positive, negative) &&
                      testBackendReplacementPreservesCounters(positive);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
