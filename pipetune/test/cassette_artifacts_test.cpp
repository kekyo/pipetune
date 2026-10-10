/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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

static std::vector<float> render(pipetune::DspPipeline &pipeline,
                                 const std::vector<float> &input, std::uint32_t channels,
                                 std::uint32_t rate, std::uint32_t blockFrames) {
  const auto frames = static_cast<std::uint32_t>(input.size() / channels);
  auto output = std::vector<float>(input.size());
  auto block = std::vector<float>(channels * blockFrames);
  for (auto start = 0u; start < frames;) {
    const auto count = std::min(blockFrames, frames - start);
    for (auto ch = 0u; ch < channels; ++ch)
      std::copy_n(input.data() + ch * frames + start, count, block.data() + ch * count);
    if (!check(pipeline.process(std::span(block.data(), channels * count), channels, count,
                                start / static_cast<double>(rate)) == pipetune::ProcessStatus::ok,
               "Cassette must process every partial block")) return {};
    for (auto ch = 0u; ch < channels; ++ch)
      std::copy_n(block.data() + ch * count, count, output.data() + ch * frames + start);
    start += count;
  }
  return output;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  constexpr auto frames = 12001u;
  struct Scenario {
    std::uint32_t rate;
    std::uint32_t channels;
    std::uint32_t first;
    std::uint32_t width;
    const char *suffix;
  };
  const auto scenarios = std::array{
      Scenario{44100, 1, 0, 1, ""}, Scenario{48000, 2, 0, 2, ""},
      Scenario{96000, 6, 0, 6, ""}, Scenario{192000, 6, 2, 2, "-pair"}};
  const auto discovered = pipetune::discoverDspBackends();
  auto backends = std::vector{discovered.scalar.backend};
  for (const auto &variant : discovered.simdVariants)
    if (variant.backend) backends.push_back(variant.backend);
  for (const auto &backend : backends) {
    if (!check(backend != nullptr, "Scalar must be available")) return 1;
    for (const auto &scenario : scenarios) {
      auto input = std::vector<float>(frames * scenario.channels);
      auto random = std::uint32_t{0x5ad9812c};
      for (auto ch = 0u; ch < scenario.channels; ++ch)
        for (auto frame = 0u; frame < frames; ++frame) {
          random = random * 1664525u + 1013904223u;
          input[ch * frames + frame] = static_cast<float>(
              0.17 * std::sin(2 * std::numbers::pi * (440 + ch * 127) * frame / scenario.rate) +
              0.03 * std::sin(2 * std::numbers::pi * 4000 * frame / scenario.rate) +
              0.01 * (static_cast<double>(random >> 8u) / 8388608.0 - 1));
        }
      auto outputs = std::array<std::vector<float>, 7>{};
      for (auto mode = 0u; mode < outputs.size(); ++mode) {
        const auto path = std::filesystem::path(argv[1]) /
            ("cassette-mode-" + std::to_string(mode) + scenario.suffix + ".effetune_preset");
        auto loaded = pipetune::loadDspPipeline(path,
            {static_cast<float>(scenario.rate), scenario.channels, 257}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error) ||
            !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1 &&
                       loaded.pipeline->latencyFrames() == 0,
                   "every Cassette mode must load as an active DSP without added latency")) return 1;
        outputs[mode] = render(*loaded.pipeline, input, scenario.channels, scenario.rate, 257);
        if (!check(outputs[mode].size() == input.size(), "Cassette output must have the expected size") ||
            !check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
                   "Cassette reset must succeed")) return 1;
        const auto reset = render(*loaded.pipeline, input, scenario.channels, scenario.rate, 113);
        if (!check(reset.size() == input.size(), "reset Cassette output must complete")) return 1;
        for (auto ch = 0u; ch < scenario.channels; ++ch)
          for (auto frame = 0u; frame < frames; ++frame) {
            const auto at = ch * frames + frame;
            if (!check(std::isfinite(outputs[mode][at]) &&
                           std::abs(outputs[mode][at] - reset[at]) <= 1e-5F,
                       "Cassette seed/reset must reproduce PCM across block partitions")) return 1;
            if ((ch < scenario.first || ch >= scenario.first + scenario.width) &&
                !check(outputs[mode][at] == input[at],
                       "Cassette must leave unselected channels untouched")) return 1;
          }
      }
      if (!check(outputs[5] == outputs[2] && outputs[6] == outputs[2],
                 "missing or invalid Cassette mode must use the official All default")) return 1;
      for (auto a = 0u; a < 5u; ++a)
        for (auto b = a + 1u; b < 5u; ++b) {
          auto difference = 0.0;
          for (auto i = std::size_t{0}; i < outputs[a].size(); ++i)
            difference += std::abs(outputs[a][i] - outputs[b][i]);
          if (!check(difference > 0.1, "all five Cassette modes must produce distinct audio")) return 1;
        }
    }
  }
  return 0;
}
