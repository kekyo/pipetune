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

static bool testAttackRouting(const std::filesystem::path &path) {
  struct Case {
    float rate;
    std::uint32_t channels;
    std::string_view selection;
    std::uint32_t latency;
    bool spatial;
  };
  const auto cases = std::array{
      Case{44100, 1, "All", 5120, false}, Case{48000, 2, "A", 5120, false},
      Case{96000, 6, "34", 10240, false}, Case{192000, 16, "16", 20480, false},
      Case{384000, 16, "All", 20480, false}, Case{48000, 6, "3", 7680, true}};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto &testCase : cases) {
      {
        auto file = std::ofstream(path);
        file << "{\"pipeline\":[{\"name\":\"Attack Tonal Balance\",\"channel\":\""
             << testCase.selection << "\"}";
        if (testCase.spatial)
          file << ",{\"name\":\"Spatial Mapper\",\"channel\":\"All\",\"parameters\":{\"ic\":16}}";
        file << "]}";
      }
      auto loaded = pipetune::loadDspPipeline(path,
          {.sampleRate = testCase.rate, .maxChannels = testCase.channels, .maxFrames = 257}, backend);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->latencyFrames() == testCase.latency,
                 "Attack routing must report the actual rate-dependent aggregate latency")) return false;
      const auto frames = testCase.latency + 4099u;
      for (auto repeat = 0u; repeat < 3u; ++repeat) {
        if (repeat == 1 && !check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
                                 "Attack reset must succeed")) return false;
        if (repeat == 2) {
          std::filesystem::remove(path);
          loaded = pipetune::rebuildDspPipeline(*loaded.pipeline,
              {.sampleRate = testCase.rate, .maxChannels = testCase.channels, .maxFrames = 257},
              backend == backends.scalar.backend ? backends.simd.backend : backends.scalar.backend);
          if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
        }
        for (auto start = 0u; start < frames;) {
          const auto count = std::min(repeat == 1 ? 63u : 257u, frames - start);
          auto audio = std::vector<float>(testCase.channels * count);
          for (auto ch = 0u; ch < testCase.channels; ++ch)
            for (auto i = 0u; i < count; ++i)
              if (start + i == 173u) audio[ch * count + i] = 0.1F / (ch + 1u);
          if (!check(loaded.pipeline->process(audio, testCase.channels, count, start / testCase.rate) ==
                         pipetune::ProcessStatus::ok, "Attack must process routed PCM")) return false;
          for (auto ch = 0u; ch < testCase.channels; ++ch) {
            for (auto i = 0u; i < count; ++i) {
              const auto expected = start + i == testCase.latency + 173u ? 0.1F / (ch + 1u) : 0.0F;
              if (!check(std::abs(audio[ch * count + i] - expected) < 2.0e-5F,
                         "Attack and untouched channels must retain aligned PCM after reset/rebuild")) return false;
            }
          }
          start += count;
        }
      }
      const auto nextRate = testCase.rate == 48000 ? 96000.0F : 48000.0F;
      loaded = pipetune::rebuildDspPipeline(*loaded.pipeline,
          {.sampleRate = nextRate, .maxChannels = testCase.channels, .maxFrames = 257});
      const auto expectedLatency = (nextRate == 48000 ? 5120u : 10240u) +
          (testCase.spatial ? (nextRate == 48000 ? 2560u : 5120u) : 0u);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.pipeline->latencyFrames() == expectedLatency,
                 "Attack rate rebuild must refresh the reported delay")) return false;
    }
  }
  return true;
}

static bool testAttackComponents(const std::filesystem::path &path) {
  constexpr auto frames = 48001u;
  const auto settings = std::array<std::string_view, 9>{
      "{}", R"({"at":12})", R"({"at":-12})", R"({"tn":12})", R"({"tn":-12})",
      R"({"ae":false})", R"({"te":false})", R"({"ae":false,"te":false})",
      R"({"at":12,"tn":-12,"ae":false,"te":false})"};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto signal : {0u, 1u, 2u}) {
      auto outputs = std::array<std::vector<float>, settings.size()>{};
      auto energies = std::array<double, settings.size()>{};
      for (auto setting = 0u; setting < settings.size(); ++setting) {
        {
          auto file = std::ofstream(path);
          file << "{\"pipeline\":[{\"name\":\"Attack Tonal Balance\",\"channel\":\"All\",\"parameters\":"
               << settings[setting] << "}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path,
            {.sampleRate = 48000, .maxChannels = 1, .maxFrames = 127}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
        auto &output = outputs[setting];
        output.reserve(frames);
        for (auto start = 0u; start < frames;) {
          const auto count = std::min(127u, frames - start);
          auto audio = std::vector<float>(count);
          for (auto i = 0u; i < count; ++i) {
            const auto position = start + i;
            audio[i] = signal == 1 ? 0.0F :
                static_cast<float>(0.1 * std::sin(2 * std::numbers::pi * 1000 * position / 48000.0));
            if (signal != 0 && position % 12000u == 100u) audio[i] += 0.4F;
          }
          if (!check(loaded.pipeline->process(audio, 1, count, start / 48000.0) ==
                         pipetune::ProcessStatus::ok, "Attack component processing failed")) return false;
          output.insert(output.end(), audio.begin(), audio.end());
          for (auto i = 0u; i < count; ++i)
            if (start + i >= 24000) energies[setting] += audio[i] * audio[i];
          start += count;
        }
      }
      if (signal != 2) {
        const auto boost = signal == 0 ? 3u : 1u;
        const auto cut = signal == 0 ? 4u : 2u;
        const auto disabled = signal == 0 ? 6u : 5u;
        if (!check(energies[boost] > energies[0] * 10 && energies[cut] < energies[0] * 0.15 &&
                       energies[disabled] < energies[0] * 0.01,
                   "Attack and Tonal controls must act on their respective signal components")) return false;
      } else {
        if (!check(energies[7] > 1.0e-8 && outputs[7] == outputs[8],
                   "residual audio must remain, independently of disabled component gains")) return false;
        for (auto i = 0u; i < frames; ++i)
          if (!check(std::isfinite(outputs[7][i]) &&
                         std::abs(outputs[0][i] - outputs[5][i] - outputs[6][i] + outputs[7][i]) < 2.0e-5F,
                     "Attack/Tonal/residual components must reconstruct neutral PCM")) return false;
      }
    }
  }
  return true;
}

static bool testBassRatesAndChannels(const std::filesystem::path &path) {
  struct Case {
    float rate;
    std::uint32_t channels;
    std::string_view selection;
    std::uint32_t first;
    std::uint32_t width;
  };
  const auto cases = std::array{
      Case{44100, 1, "All", 0, 1}, Case{48000, 2, "A", 0, 2},
      Case{88200, 6, "34", 2, 2}, Case{96000, 16, "15", 14, 1},
      Case{176400, 16, "1516", 14, 2}, Case{192000, 1, "1", 0, 1}};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto &testCase : cases) {
      {
        auto file = std::ofstream(path);
        file << "{\"pipeline\":[{\"name\":\"Bass Extender\",\"channel\":\"" << testCase.selection
             << "\",\"parameters\":{\"am\":100}}]}";
      }
      auto loaded = pipetune::loadDspPipeline(path,
          {.sampleRate = testCase.rate, .maxChannels = testCase.channels, .maxFrames = 257}, backend);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->latencyFrames() == 0,
                 "Bass Extender must execute supported rates without delay")) return false;
      const auto frames = static_cast<std::uint32_t>(testCase.rate) + 1u;
      auto reference = std::vector<float>{};
      for (auto repeat = 0u; repeat < 2u; ++repeat) {
        if (repeat != 0 && !check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
                                  "Bass reset must succeed")) return false;
        auto re = 0.0;
        auto im = 0.0;
        auto weight = 0.0;
        for (auto start = 0u; start < frames;) {
          const auto count = std::min(repeat == 0 ? 257u : 63u, frames - start);
          auto audio = std::vector<float>(testCase.channels * count);
          for (auto ch = 0u; ch < testCase.channels; ++ch)
            for (auto i = 0u; i < count; ++i)
              audio[ch * count + i] = static_cast<float>(0.25 *
                  std::sin(2 * std::numbers::pi * 100 * (start + i) / testCase.rate));
          const auto input = audio;
          if (!check(loaded.pipeline->process(audio, testCase.channels, count, start / testCase.rate) ==
                         pipetune::ProcessStatus::ok, "Bass processing failed")) return false;
          for (auto ch = 0u; ch < testCase.channels; ++ch) {
            for (auto i = 0u; i < count; ++i) {
              const auto generated = audio[ch * count + i] - input[ch * count + i];
              if (ch < testCase.first || ch >= testCase.first + testCase.width) {
                if (!check(generated == 0, "Bass must preserve channels outside the selected route")) return false;
              } else if (ch != testCase.first &&
                         !check(std::abs(generated - (audio[testCase.first * count + i] - input[testCase.first * count + i])) < 1.0e-7F,
                                "Bass generated signal must be shared by the selected pair")) return false;
              if (ch != testCase.first) continue;
              if (repeat == 0) reference.push_back(audio[ch * count + i]);
              else if (!check(reference[start + i] == audio[ch * count + i],
                              "Bass reset and block partitioning must reproduce PCM exactly")) return false;
              if (start + i >= frames / 2u) {
                const auto window = 0.5 - 0.5 * std::cos(2 * std::numbers::pi *
                    (start + i - frames / 2u) / (frames - frames / 2u));
                const auto phase = 2 * std::numbers::pi * 50 * (start + i) / testCase.rate;
                re += window * generated * std::cos(phase);
                im += window * generated * std::sin(phase);
                weight += window;
              }
            }
          }
          start += count;
        }
        const auto amplitude = 2 * std::hypot(re, im) / weight;
        if (!check(amplitude > 0.105 && amplitude < 0.24,
                   "Bass must generate the octave below a 100 Hz tone at every supported rate")) return false;
      }
      for (const auto unsupported : {32000.0F, 352800.0F, 384000.0F}) {
        const auto rejected = pipetune::rebuildDspPipeline(*loaded.pipeline,
            {.sampleRate = unsupported, .maxChannels = testCase.channels, .maxFrames = 257});
        if (!check(rejected.pipeline == nullptr && !rejected.error.empty() && rejected.warnings.empty(),
                   "unsupported Bass rates must fail without silently omitting the DSP")) return false;
      }
      auto silence = std::vector<float>(testCase.channels * 257u, 0.0F);
      if (!check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok &&
                     loaded.pipeline->process(silence, testCase.channels, 257, 0.0) == pipetune::ProcessStatus::ok &&
                     std::ranges::all_of(silence, [](float v) { return v == 0; }),
                 "Bass must reset to silence and remain usable after rejected rebuilds")) return false;
    }
  }
  return true;
}

static bool testBassMix(const std::filesystem::path &path) {
  for (const auto antiPhase : {false, true}) {
    for (const auto amount : {0, 100}) {
      for (const auto gain : {0, -6}) {
        {
          auto file = std::ofstream(path);
          file << "{\"pipeline\":[{\"name\":\"Bass Extender\",\"channel\":\"All\",\"parameters\":{\"am\":"
               << amount << ",\"og\":" << gain << "}}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path,
            {.sampleRate = 48000, .maxChannels = 2, .maxFrames = 127});
        if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
        for (auto start = 0u; start < 24000u; start += 127u) {
          auto audio = std::vector<float>(254u);
          for (auto i = 0u; i < 127u; ++i) {
            audio[i] = static_cast<float>(0.2 * std::sin(2 * std::numbers::pi * 100 * (start + i) / 48000.0));
            audio[127u + i] = audio[i] * (antiPhase ? -1.0F : 0.5F);
          }
          const auto input = audio;
          if (!check(loaded.pipeline->process(audio, 2, 127, start / 48000.0) == pipetune::ProcessStatus::ok,
                     "Bass mix processing failed")) return false;
          const auto outputGain = std::pow(10.0F, gain / 20.0F);
          for (auto i = 0u; i < 127u; ++i) {
            if (antiPhase || amount == 0) {
              for (auto ch = 0u; ch < 2u; ++ch)
                if (!check(std::abs(audio[ch * 127u + i] - input[ch * 127u + i] * outputGain) < 1.0e-7F,
                           "zero amount or antiphase Bass must retain dry audio with output gain")) return false;
            } else if (!check(std::abs((audio[i] - input[i] * outputGain) -
                                      (audio[127u + i] - input[127u + i] * outputGain)) < 1.0e-7F,
                              "asymmetric stereo must receive identical generated bass")) return false;
          }
        }
      }
    }
  }
  return true;
}

int main() {
  const auto path = std::filesystem::temp_directory_path() /
      ("pipetune-attack-bass-" + std::to_string(getpid()) + ".effetune_preset");
  const auto passed = testAttackRouting(path) && testAttackComponents(path) &&
      testBassRatesAndChannels(path) && testBassMix(path);
  std::filesystem::remove(path);
  return passed ? 0 : 1;
}
