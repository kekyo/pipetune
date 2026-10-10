/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_pipeline_slot.h"
#include <pipetune/dsp_backend.h>
#include <pipetune/dsp_pipeline.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <numbers>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static std::vector<float> signal(std::uint32_t rate, std::uint32_t channels,
                                std::uint32_t frames, std::uint32_t first,
                                std::uint32_t width) {
  auto audio = std::vector<float>(static_cast<std::size_t>(channels) * frames);
  for (auto ch = 0u; ch < channels; ++ch)
    for (auto i = 0u; i < frames; ++i)
      audio[ch * frames + i] = ch >= first && ch < first + width
          ? static_cast<float>(0.25 * std::sin(2 * std::numbers::pi *
                (440 + 173 * (ch - first)) * i / rate))
          : 0.015625F * static_cast<float>(ch + 1u);
  return audio;
}

static std::vector<float> render(pipetune::DspPipeline &pipeline,
                                 const std::vector<float> &input, std::uint32_t channels,
                                 std::uint32_t rate, std::uint32_t blockFrames) {
  const auto frames = static_cast<std::uint32_t>(input.size() / channels);
  auto output = std::vector<float>(input.size());
  auto audio = std::vector<float>(channels * blockFrames);
  for (auto start = 0u; start < frames;) {
    const auto count = std::min(blockFrames, frames - start);
    for (auto ch = 0u; ch < channels; ++ch)
      std::copy_n(input.data() + ch * frames + start, count, audio.data() + ch * count);
    if (!check(pipeline.process(std::span(audio.data(), channels * count), channels, count,
                                start / static_cast<double>(rate)) == pipetune::ProcessStatus::ok,
               "Adaptive Prediction must process each partial block")) return {};
    for (auto ch = 0u; ch < channels; ++ch)
      std::copy_n(audio.data() + ch * count, count, output.data() + ch * frames + start);
    start += count;
  }
  return output;
}

static bool testWidth(const std::filesystem::path &directory,
                      const std::shared_ptr<const pipetune::DspBackend> &backend) {
  for (const auto channels : {3u, 6u, 16u}) {
    const auto loaded = pipetune::loadDspPipeline(
        directory / "adaptive-residual.effetune_preset", {48000, channels, 257}, backend);
    if (!check(loaded.pipeline == nullptr &&
                   loaded.error.find("Adaptive Prediction requires one or two selected processing channels") !=
                       std::string::npos,
               "an enabled Adaptive All selection wider than stereo must fail explicitly")) return false;
    for (const auto file : {"adaptive-disabled.effetune_preset",
                            "adaptive-disabled-section.effetune_preset"}) {
      const auto disabled = pipetune::loadDspPipeline(directory / file, {48000, channels, 257}, backend);
      if (!check(disabled.pipeline != nullptr && disabled.warnings.empty() &&
                     disabled.pipeline->activePluginCount() == 0,
                 "disabled Adaptive nodes and sections must remain off at any bus width")) return false;
    }
  }
  return true;
}

static bool testLearning(const std::filesystem::path &directory,
                         const std::shared_ptr<const pipetune::DspBackend> &backend) {
  struct Scenario {
    const char *preset;
    std::uint32_t rate;
    std::uint32_t channels;
    std::uint32_t first;
    std::uint32_t width;
  };
  const auto scenarios = std::array{
      Scenario{"adaptive-residual", 44100, 2, 0, 2},
      Scenario{"adaptive-residual", 48000, 1, 0, 1},
      Scenario{"adaptive-residual", 96000, 2, 0, 2},
      Scenario{"adaptive-residual", 192000, 2, 0, 2},
      Scenario{"adaptive-residual", 384000, 2, 0, 2},
      Scenario{"adaptive-last-pair", 48000, 16, 14, 2},
      Scenario{"adaptive-last-single", 48000, 16, 15, 1}};
  for (const auto &scenario : scenarios) {
    const auto frames = scenario.rate * (scenario.channels == 1 ? 4u : 2u) + 13u;
    const auto input = signal(scenario.rate, scenario.channels, frames, scenario.first, scenario.width);
    const auto path = directory / (std::string(scenario.preset) + ".effetune_preset");
    auto loaded = pipetune::loadDspPipeline(path,
        {static_cast<float>(scenario.rate), scenario.channels, 257}, backend);
    if (!check(loaded.pipeline != nullptr, loaded.error) ||
        !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1 &&
                   loaded.pipeline->latencyFrames() == 0,
               "Adaptive learning must have no warnings or reporting latency")) return false;
    const auto output = render(*loaded.pipeline, input, scenario.channels, scenario.rate, 257);
    if (!check(output.size() == input.size(), "Adaptive output dimensions must be retained")) return false;
    for (auto ch = 0u; ch < scenario.channels; ++ch) {
      auto inputEnergy = 0.0;
      auto outputEnergy = 0.0;
      for (auto i = 0u; i < frames; ++i) {
        const auto at = static_cast<std::size_t>(ch) * frames + i;
        if (!check(std::isfinite(output[at]), "learning must produce finite PCM")) return false;
        if (ch < scenario.first || ch >= scenario.first + scenario.width) {
          if (!check(output[at] == input[at], "unselected channels must preserve their PCM")) return false;
        } else if (i >= frames * 3u / 4u) {
          inputEnergy += static_cast<double>(input[at]) * input[at];
          outputEnergy += static_cast<double>(output[at]) * output[at];
        }
      }
      if (inputEnergy > 0) {
        std::cout << "Adaptive " << scenario.rate << " Hz channel " << ch + 1u
                  << " residual " << 10 * std::log10(outputEnergy / inputEnergy) << " dB\n";
        if (!check(outputEnergy < inputEnergy * (scenario.channels == 1 ? 0.00316228 : 0.01),
                   "periodic input must be learned independently in every selected channel")) return false;
      }
    }
    if (!check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok, "Adaptive reset must succeed"))
      return false;
    const auto reset = render(*loaded.pipeline, input, scenario.channels, scenario.rate, 113);
    if (!check(reset == output, "reset must relearn from scratch independently of block partition"))
      return false;
  }

  constexpr auto rate = 48000u;
  constexpr auto frames = rate * 4u + 13u;
  const auto input = signal(rate, 2, frames, 0, 2);
  auto residual = pipetune::loadDspPipeline(directory / "adaptive-residual.effetune_preset",
                                           {rate, 2, 257}, backend);
  auto prediction = pipetune::loadDspPipeline(directory / "adaptive-prediction.effetune_preset",
                                             {rate, 2, 257}, backend);
  auto complement = pipetune::loadDspPipeline(directory / "adaptive-complementary.effetune_preset",
                                             {rate, 2, 257}, backend);
  if (!check(residual.pipeline && prediction.pipeline && complement.pipeline,
             residual.error + prediction.error + complement.error)) return false;
  const auto a = render(*residual.pipeline, input, 2, rate, 257);
  const auto b = render(*prediction.pipeline, input, 2, rate, 127);
  const auto c = render(*complement.pipeline, input, 2, rate, 113);
  if (!check(a.size() == input.size() && b.size() == input.size() && c.size() == input.size(),
             "complementary rendering must complete")) return false;
  auto predictedEnergy = 0.0;
  for (auto i = std::size_t{0}; i < input.size(); ++i) {
    if (!check(std::abs(a[i] + b[i] - input[i]) < 2e-6F && c[i] == input[i],
               "prediction and residual must reconstruct the original signal")) return false;
    predictedEnergy += static_cast<double>(b[i]) * b[i];
  }
  if (!check(predictedEnergy > frames * 0.01, "the learned predictor must generate audible PCM"))
    return false;
  for (auto ch = 0u; ch < 2u; ++ch) {
    auto mono = pipetune::loadDspPipeline(directory / "adaptive-residual.effetune_preset",
                                         {rate, 1, 257}, backend);
    if (!check(mono.pipeline != nullptr, mono.error)) return false;
    const auto channelInput = std::vector<float>(input.begin() + ch * frames,
                                                 input.begin() + (ch + 1u) * frames);
    const auto isolated = render(*mono.pipeline, channelInput, 1, rate, 257);
    if (!check(isolated.size() == frames &&
                   std::equal(isolated.begin(), isolated.end(), a.begin() + ch * frames),
               "stereo learning must match two completely independent mono predictors")) return false;
  }

  for (const auto file : {"adaptive-hold.effetune_preset", "adaptive-freeze.effetune_preset"}) {
    auto cold = pipetune::loadDspPipeline(directory / file, {rate, 2, 257}, backend);
    if (!check(cold.pipeline != nullptr, cold.error)) return false;
    const auto output = render(*cold.pipeline, signal(rate, 2, 8193, 0, 2), 2, rate, 257);
    if (!check(output.size() == 16386 &&
                   std::ranges::all_of(output, [](float value) { return value == 0.0F; }),
               "Hold or Freeze before learning must not invent a learned sound")) return false;
  }
  return true;
}

static bool testIdleAndRebuild(const std::filesystem::path &directory,
                               const std::shared_ptr<const pipetune::DspBackend> &backend) {
  constexpr auto rate = 48000u;
  constexpr auto block = 128u;
  const auto input = signal(rate, 2, rate * 2u, 0, 2);
  const auto path = directory / "adaptive-autonomy.effetune_preset";
  for (const auto timeout : {0u, 100u}) {
    auto loaded = pipetune::loadDspPipeline(path, {rate, 2, block}, backend);
    auto reference = pipetune::loadDspPipeline(path, {rate, 2, block}, backend);
    if (!check(loaded.pipeline && reference.pipeline, loaded.error + reference.error)) return false;
    auto slot = pipetune::DspPipelineSlot(std::move(loaded.pipeline));
    auto generatedEnergy = 0.0;
    auto position = 0u;
    for (auto phase = 0u; phase < 3u; ++phase) {
      if (phase == 2u && timeout != 0u &&
          !check(reference.pipeline->reset() == pipetune::ProcessStatus::ok, "idle reference must reset"))
        return false;
      const auto frames = phase == 0u ? rate * 2u : phase == 1u ? 9600u : 1024u;
      for (auto start = 0u; start < frames; start += block, position += block) {
        auto audio = std::array<float, block * 2u>{};
        if (phase != 1u)
          for (auto ch = 0u; ch < 2u; ++ch)
            std::copy_n(input.data() + ch * rate * 2u + start, block, audio.data() + ch * block);
        auto expected = audio;
        const auto before = slot.performanceCounters();
        const auto actual = slot.processWithIdle(audio, 2, block, position / static_cast<double>(rate),
                                                 {.timeoutMilliseconds = timeout});
        if (!check(actual.status == pipetune::ProcessStatus::ok &&
                       reference.pipeline->process(expected, 2, block, position / static_cast<double>(rate)) ==
                           pipetune::ProcessStatus::ok, "Adaptive idle processing must succeed"))
          return false;
        if (phase != 1u || timeout == 0u)
          if (!check(audio == expected, "idle off must retain learning; wake must match a fresh reset"))
            return false;
        if (phase == 1u && start >= 5120u) {
          if (!check(timeout == 0u ?
                         actual.activity == pipetune::DspActivity::active &&
                             slot.performanceCounters().processedFrames == before.processedFrames + block :
                         actual.activity == pipetune::DspActivity::sleeping &&
                             slot.performanceCounters().processedFrames == before.processedFrames &&
                             std::ranges::all_of(audio, [](float value) { return value == 0.0F; }),
                     "idle policy must control both generated sound and actual DSP work")) return false;
        }
        if (phase == 1u && start < 4096u)
          for (const auto value : audio) generatedEnergy += static_cast<double>(value) * value;
      }
    }
    if (!check(generatedEnergy > 1e-5, "trained autonomous feedback must persist after input stops"))
      return false;
    const auto alternatives = pipetune::discoverDspBackends();
    const auto replacementBackend = backend->kind() == pipetune::DspBackendKind::scalar && alternatives.simd.backend
        ? alternatives.simd.backend : alternatives.scalar.backend;
    for (const auto newRate : {48000u, 96000u}) {
      const auto options = pipetune::PipelineBuildOptions{static_cast<float>(newRate), 2, block};
      auto rebuilt = slot.rebuildActive(options, replacementBackend);
      auto fresh = pipetune::loadDspPipeline(path, options, replacementBackend);
      if (!check(rebuilt.pipeline && fresh.pipeline, rebuilt.error + fresh.error)) return false;
      const auto probe = signal(newRate, 2, 1025, 0, 2);
      if (!check(render(*rebuilt.pipeline, probe, 2, newRate, 127) ==
                     render(*fresh.pipeline, probe, 2, newRate, 113),
                 "rate/backend reconstruction must start with a new learning state")) return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  const auto discovered = pipetune::discoverDspBackends();
  auto backends = std::vector{discovered.scalar.backend};
  for (const auto &variant : discovered.simdVariants)
    if (variant.backend) backends.push_back(variant.backend);
  for (const auto &backend : backends)
    if (!check(backend != nullptr, "Scalar must be available") ||
        !testWidth(argv[1], backend) || !testLearning(argv[1], backend) ||
        !testIdleAndRebuild(argv[1], backend)) return 1;
  return 0;
}
