/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/dsp_pipeline.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <string>
#include <unistd.h>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

// Integer-frequency partials make the final one-second spectral comparison exact.
// The envelope and quiet intervals exercise measurement over time.
static std::vector<float> programme(std::uint32_t rate) {
  auto table = std::vector<float>(rate);
  for (const auto frequency : {63, 100, 160, 250, 400, 630, 1000, 1600, 2500, 4000, 6300, 10000})
    for (auto i = 0u; i < rate; ++i)
      table[i] += static_cast<float>((frequency >= 1000 ? 0.025 : 0.008) *
          std::sin(2 * std::numbers::pi * frequency * i / rate + frequency * 0.137));
  return table;
}

static double trebleRatio(const std::vector<float> &audio, std::uint32_t rate) {
  auto low = 0.0;
  auto high = 0.0;
  for (const auto frequency : {100, 250, 400, 2500, 4000, 6300}) {
    auto re = 0.0;
    auto im = 0.0;
    for (auto i = 0u; i < rate; ++i) {
      const auto phase = 2 * std::numbers::pi * frequency * i / rate;
      re += audio[audio.size() - rate + i] * std::cos(phase);
      im += audio[audio.size() - rate + i] * std::sin(phase);
    }
    (frequency < 1000 ? low : high) += re * re + im * im;
  }
  return high / low;
}

static bool testNeutralRates(const std::filesystem::path &path) {
  struct Case { float rate; std::uint32_t channels; std::string_view selection; };
  const auto cases = std::array{
      Case{32000, 1, "All"}, Case{44100, 2, "All"}, Case{48000, 6, "34"},
      Case{88200, 16, "15"}, Case{96000, 6, "All"}, Case{176400, 16, "1516"},
      Case{192000, 1, "1"}, Case{384000, 2, "All"}};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto &c : cases) {
      for (const auto params : {R"({"am":0})", R"({"rg":0})", R"({"mp":true})"}) {
        {
          auto file = std::ofstream(path);
          file << "{\"pipeline\":[{\"name\":\"Tonal Balance EQ\",\"channel\":\"" << c.selection
               << "\",\"parameters\":" << params << "}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path, {c.rate, c.channels, 257}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error) ||
            !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1 &&
                       loaded.pipeline->latencyFrames() == 0, "Tonal neutral routing must be active without delay")) return false;
        for (auto start = 0u; start < 8197u;) {
          const auto count = std::min(257u, 8197u - start);
          auto audio = std::vector<float>(c.channels * count);
          for (auto ch = 0u; ch < c.channels; ++ch)
            for (auto i = 0u; i < count; ++i)
              audio[ch * count + i] = static_cast<float>(0.1 * std::sin(
                  2 * std::numbers::pi * (137 + ch * 31) * (start + i) / c.rate));
          const auto input = audio;
          if (!check(loaded.pipeline->process(audio, c.channels, count, start / c.rate) ==
                         pipetune::ProcessStatus::ok, "Tonal neutral processing failed")) return false;
          for (auto i = std::size_t{0}; i < audio.size(); ++i)
            if (!check(std::abs(audio[i] - input[i]) < 2e-6F,
                       "zero amount/range or initially paused measurement must preserve PCM")) return false;
          start += count;
        }
      }
    }
  }
  return true;
}

static bool testCorrection(const std::filesystem::path &path) {
  constexpr auto rate = 48000u;
  constexpr auto frames = rate * 32u + 13u;
  const auto table = programme(rate);
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    auto ratios = std::array<double, 5>{};
    const auto settings = std::array<std::string_view, 5>{
        R"({"tg":"All","at":1})",
        R"({"tg":"Tilt","ts":-12,"at":1,"rg":12})",
        R"({"tg":"All","at":1,"ea4":true,"ta4":"hs","fa4":3000,"ga4":12,"qa4":0.7})",
        R"({"tg":"Tilt","ts":-12,"at":100,"rg":12})",
        R"({"tg":"Tilt","ts":-12,"at":1,"rg":12,"am":0})"};
    for (auto setting = 0u; setting < settings.size(); ++setting) {
      {
        auto file = std::ofstream(path);
        file << "{\"pipeline\":[{\"name\":\"Tonal Balance EQ\",\"channel\":\"34\",\"parameters\":"
             << settings[setting] << "}]}";
      }
      auto loaded = pipetune::loadDspPipeline(path, {rate, 6, 257}, backend);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->latencyFrames() == 0,
                 "Tonal correction must load without warnings or delay")) return false;
      auto reference = std::vector<float>{};
      auto original = std::vector<float>{};
      // Re-run an established correction after reset with different block boundaries.
      const auto repeats = setting == 1 ? 2u : 1u;
      for (auto repeat = 0u; repeat < repeats; ++repeat) {
        if (repeat != 0 && !check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
                                  "Tonal reset failed")) return false;
        for (auto start = 0u; start < frames;) {
          const auto count = std::min(repeat == 0 ? 257u : 113u, frames - start);
          auto audio = std::vector<float>(6u * count);
          for (auto i = 0u; i < count; ++i) {
            const auto pos = start + i;
            const auto t = pos / static_cast<double>(rate);
            const auto envelope = t >= 12 && t < 13 ? 0.0 :
                t >= 13 && t < 14 ? 1e-6 : 0.7 + 0.3 * std::cos(2 * std::numbers::pi * t);
            const auto value = static_cast<float>(table[pos % rate] * envelope);
            for (auto ch = 0u; ch < 6u; ++ch) audio[ch * count + i] = value;
          }
          const auto input = audio;
          if (!check(loaded.pipeline->process(audio, 6, count, start / static_cast<double>(rate)) ==
                         pipetune::ProcessStatus::ok, "Tonal correction processing failed")) return false;
          for (auto i = 0u; i < count; ++i) {
            if (!check(std::isfinite(audio[2u * count + i]) && audio[2u * count + i] == audio[3u * count + i],
                       "Tonal must apply one finite correction to both selected channels")) return false;
            for (const auto ch : {0u, 1u, 4u, 5u})
              if (!check(audio[ch * count + i] == input[ch * count + i],
                         "Tonal must preserve unselected channels exactly")) return false;
            if (repeat == 0) {
              reference.push_back(audio[2u * count + i]);
              original.push_back(input[2u * count + i]);
            } else if (!check(std::abs(reference[start + i] - audio[2u * count + i]) < 2e-5F,
                              "Tonal reset and block partitioning must reproduce the correction")) return false;
          }
          start += count;
        }
      }
      ratios[setting] = trebleRatio(reference, rate) / trebleRatio(original, rate);
      std::cout << "Tonal setting=" << setting << " corrected/input treble ratio=" << ratios[setting] << '\n';
      if (setting == 4)
        for (auto i = std::size_t{0}; i < reference.size(); ++i)
          if (!check(std::abs(reference[i] - original[i]) < 2e-6F,
                     "amount zero must stay neutral after long measurement")) return false;
    }
    if (!check(ratios[1] < 0.7 && ratios[3] < 0.7,
               "finite and cumulative Tilt measurement must reduce excess treble") ||
        !check(ratios[2] > ratios[0] * 1.1,
               "Target Adjust must change the established spectral correction")) return false;
  }
  return true;
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
      ("pipetune-tonal-test-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto passed = testNeutralRates(directory / "neutral.effetune_preset") &&
      testCorrection(directory / "correction.effetune_preset");
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
