#include "dsp_pipeline_slot.h"

#include "pipetune/dsp_pipeline.h"

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

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-slot-test-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto positive =
      writeVolumePreset(directory, "positive.effetune_preset", 6);
  const auto negative =
      writeVolumePreset(directory, "negative.effetune_preset", -6);

  const auto passed = testReplacementChangesPcm(positive, negative) &&
                      testBypassDoesNotReportDspWork() &&
                      testConcurrentReplacementProducesOnlyCompletePipelines(
                          positive, negative);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
