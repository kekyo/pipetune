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

// A deterministic 120 BPM kick/snare pattern with eighth-note hats. This uses
// only synthesized PCM, and exercises the product path with telemetry disabled.
static std::vector<float> groove(std::uint32_t rate) {
  auto audio = std::vector<float>(rate);
  auto state = std::uint32_t{0x12481248};
  auto previous = 0.0;
  for (auto i = 0u; i < rate; ++i) {
    state = state * 1664525u + 1013904223u;
    const auto noise = static_cast<double>(state >> 8u) / 8388608.0 - 1.0;
    const auto t = i / static_cast<double>(rate);
    const auto kick = 0.2 * std::sin(2 * std::numbers::pi *
        (50 * t + 2.7 * (1 - std::exp(-t / 0.03)))) * std::exp(-t / 0.12);
    const auto snareTime = t - 0.5;
    const auto snare = snareTime < 0 ? 0.0 :
        (0.13 * noise + 0.08 * std::sin(2 * std::numbers::pi * 190 * snareTime)) *
        std::exp(-snareTime / 0.06);
    const auto hat = 0.04 * (noise - previous) * std::exp(-std::fmod(t, 0.25) / 0.02);
    audio[i] = static_cast<float>(kick + snare + hat);
    previous = noise;
  }
  return audio;
}

static bool testClicks(const std::filesystem::path &path) {
  struct Case {
    std::uint32_t rate;
    std::uint32_t channels;
    std::string_view selection;
    std::uint32_t first;
    std::uint32_t width;
  };
  const auto cases = std::array{
      Case{48000, 4, "All", 0, 2}, Case{32000, 1, "All", 0, 1},
      Case{44100, 2, "All", 0, 2}, Case{88200, 4, "34", 2, 2},
      Case{96000, 16, "15", 14, 1}, Case{176400, 16, "1516", 14, 2},
      Case{192000, 1, "1", 0, 1}, Case{384000, 2, "All", 0, 2}};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto &c : cases) {
      const auto table = groove(c.rate);
      auto reference = std::vector<float>{};
      for (const auto click : {true, false}) {
        {
          auto file = std::ofstream(path);
          file << "{\"pipeline\":[{\"name\":\"Rhythm Analyzer\",\"channel\":\"" << c.selection
               << "\",\"parameters\":{\"mn\":100,\"mx\":140,\"ck\":" << (click ? "true" : "false")
               << ",\"sp\":16,\"vt\":false,\"vm\":false,\"ve\":false,\"vl\":false}}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path,
            {static_cast<float>(c.rate), c.channels, 257}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error) ||
            !check(loaded.warnings.empty() && loaded.pipeline->latencyFrames() == 0 &&
                       loaded.pipeline->activePluginCount() == 1 &&
                       loaded.pipeline->presetEntries()[0].state == pipetune::PresetEntryState::enabled,
                   "Rhythm must execute even with clicks and displays disabled")) return false;
        const auto repeats = click && c.rate == 48000 ? 2u : 1u;
        for (auto repeat = 0u; repeat < repeats; ++repeat) {
          if (repeat != 0 && !check(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
                                    "Rhythm reset failed")) return false;
          const auto frames = c.rate * (click ? 20u : 2u) + 13u;
          auto onsets = std::vector<double>{};
          auto lastChanged = -1.0;
          for (auto start = 0u; start < frames;) {
            const auto count = std::min(repeat == 0 ? 257u : 113u, frames - start);
            auto audio = std::vector<float>(c.channels * count);
            for (auto ch = 0u; ch < c.channels; ++ch)
              for (auto i = 0u; i < count; ++i)
                audio[ch * count + i] = start + i < c.rate / 2u ? 0.0F :
                    table[(start + i - c.rate / 2u) % c.rate];
            const auto input = audio;
            if (!check(loaded.pipeline->process(audio, c.channels, count,
                           start / static_cast<double>(c.rate)) == pipetune::ProcessStatus::ok,
                       "Rhythm processing failed")) return false;
            for (auto i = 0u; i < count; ++i) {
              const auto added = audio[c.first * count + i] - input[c.first * count + i];
              for (auto ch = 0u; ch < c.channels; ++ch) {
                const auto selected = click && ch >= c.first && ch < c.first + c.width;
                if (!check(std::isfinite(audio[ch * count + i]) &&
                               (selected ? audio[ch * count + i] == audio[c.first * count + i] :
                                           audio[ch * count + i] == input[ch * count + i]),
                           "clicks must match on selected channels and leave all other PCM unchanged")) return false;
              }
              if (click && c.rate == 48000) {
                if (repeat == 0) reference.push_back(added);
                else if (!check(std::abs(reference[start + i] - added) < 1e-6F,
                                 "reset must reproduce detection and clicks across block boundaries")) return false;
              }
              if (std::abs(added) > 1e-6F) {
                const auto time = (start + i) / static_cast<double>(c.rate);
                if (lastChanged < 0 || time - lastChanged > 0.07) onsets.push_back(time);
                lastChanged = time;
              }
            }
            start += count;
          }
          if (click) {
            std::cout << "Rhythm rate=" << c.rate << " channels=" << c.channels << " route=" << c.selection
                      << " reset=" << repeat << " clicks=" << onsets.size() << std::endl;
            if (!check(onsets.size() >= 20, "a steady groove must produce detected-beat clicks")) return false;
            for (auto i = std::size_t{1}; i < onsets.size(); ++i)
              if (onsets[i - 1] >= 8.0 &&
                  !check(std::abs(onsets[i] - onsets[i - 1] - 0.5) < 0.03,
                         "established clicks must follow the 120 BPM input")) return false;
          } else if (!check(onsets.empty(), "click-off reload must remove all generated clicks")) return false;
        }
      }
    }
  }
  return true;
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() /
      ("pipetune-rhythm-test-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto passed = testClicks(directory / "rhythm.effetune_preset");
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
