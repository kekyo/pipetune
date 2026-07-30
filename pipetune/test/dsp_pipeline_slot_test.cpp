#include "dsp_pipeline_slot.h"

#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_pipeline.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>
#include <vector>

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

static std::filesystem::path writeDelayPreset(
    const std::filesystem::path &directory, std::string_view name) {
  const auto path = directory / name;
  auto stream = std::ofstream(path, std::ios::binary);
  stream << R"json({"pipeline":[{"name":"Delay","enabled":true,"parameters":{"pd":0,"ds":1,"dp":0,"hd":20000,"ld":20,"mx":100,"fb":0,"pp":0},"channel":"A"}]})json";
  return path;
}

static std::unique_ptr<pipetune::DspPipeline> loadPipeline(
    const std::filesystem::path &path) {
  auto loaded = pipetune::loadDspPipeline(
      path, {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
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
  if (!check(slot.process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "initial slot processing failed") ||
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
  return check(slot.process(samples, 1, 1, 0.1) ==
                   pipetune::ProcessStatus::ok,
               "replacement slot processing failed") &&
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
  if (!check(slot.process(samples, 1, 32, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "bypass slot processing failed")) {
    return false;
  }
  const auto counters = slot.performanceCounters();
  return check(counters.processedFrames == 0 &&
                   counters.processingNanoseconds == 0,
               "bypass must not be counted as EffeTune DSP work");
}

static bool testResetClearsActivePipelineState(
    const std::filesystem::path &delayPath) {
  auto referencePipeline = loadPipeline(delayPath);
  auto resetPipeline = loadPipeline(delayPath);
  if (referencePipeline == nullptr || resetPipeline == nullptr) {
    return false;
  }
  auto reference = pipetune::DspPipelineSlot(std::move(referencePipeline));
  auto reset = pipetune::DspPipelineSlot(std::move(resetPipeline));
  auto impulse = std::vector<float>(32, 0.0F);
  impulse[0] = 1.0F;
  auto resetImpulse = impulse;
  if (!check(reference.process(impulse, 1, 32, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "reference delay impulse processing failed") ||
      !check(reset.process(resetImpulse, 1, 32, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "reset delay impulse processing failed") ||
      !check(reset.resetActive(), "active DSP reset failed")) {
    return false;
  }

  auto referenceTail = std::vector<float>(32, 0.0F);
  auto resetTail = std::vector<float>(32, 0.0F);
  if (!check(reference.process(referenceTail, 1, 32, 32.0 / 48000.0) ==
                 pipetune::ProcessStatus::ok,
             "reference delay tail processing failed") ||
      !check(reset.process(resetTail, 1, 32, 32.0 / 48000.0) ==
                 pipetune::ProcessStatus::ok,
             "reset delay tail processing failed")) {
    return false;
  }
  const auto referenceHasTail =
      std::ranges::any_of(referenceTail, [](float sample) {
        return std::abs(sample) > 1.0e-6F;
      });
  const auto resetIsSilent =
      std::ranges::all_of(resetTail, [](float sample) {
        return sample == 0.0F;
      });

  auto bypass = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(bypass.pipeline != nullptr, bypass.error)) {
    return false;
  }
  auto bypassSlot = pipetune::DspPipelineSlot(std::move(bypass.pipeline));
  return check(referenceHasTail,
               "unreset delay must retain its pending output") &&
         check(resetIsSilent,
               "reset delay must discard its pending output") &&
         check(bypassSlot.resetActive(),
               "bypass reset must succeed");
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
      if (slot.process(samples, 1, 1,
                       static_cast<double>(iteration) / 48000.0) !=
              pipetune::ProcessStatus::ok ||
          (!approximately(samples[0], positive) &&
           !approximately(samples[0], negative))) {
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
  const auto initialRevision = slot.revision();
  slot.stageReplacement(std::move(replacement));
  auto samples = std::vector<float>{0.25F};
  if (!check(slot.hasStagedReplacement(),
             "staged replacement must retain rollback state") ||
      !check(slot.revision() > initialRevision,
             "staging a replacement must advance the pipeline revision") ||
      !check(slot.process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok &&
                 approximately(samples[0],
                               0.25F * std::pow(10.0F, -6.0F / 20.0F)),
             "staged replacement must become active")) {
    return false;
  }

  slot.rollbackStaged();
  const auto rollbackRevision = slot.revision();
  samples[0] = 0.25F;
  if (!check(!slot.hasStagedReplacement(),
             "rollback must close the transaction") ||
      !check(rollbackRevision > initialRevision,
             "rollback must publish another pipeline revision") ||
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
               "commit must release rollback state") &&
         check(slot.revision() > rollbackRevision,
               "new staged pipeline must advance the revision");
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
  const auto delay =
      writeDelayPreset(directory, "delay.effetune_preset");

  const auto passed = testReplacementChangesPcm(positive, negative) &&
                      testBypassDoesNotReportDspWork() &&
                      testResetClearsActivePipelineState(delay) &&
                      testConcurrentReplacementProducesOnlyCompletePipelines(
                          positive, negative) &&
                      testStagedReplacementCanCommitAndRollback(
                          positive, negative) &&
                      testBackendReplacementPreservesCounters(positive);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
