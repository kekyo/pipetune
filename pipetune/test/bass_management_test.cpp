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

static bool testIirRouting(const std::filesystem::path &path) {
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto slope : {24, 48, 96}) {
      for (const auto frequency : {20.0, 1000.0}) {
        {
          auto file = std::ofstream(path);
          file << R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"ro":[1,2,3,3,0,3],"fc":[80],"sl":[)"
               << slope << R"(],"rt":[12,4],"ri":[8],"su":12,"bg":-6,"lg":-3,"hg":-6}}]})";
        }
        auto loaded = pipetune::loadDspPipeline(path,
            {.sampleRate = 48000, .maxChannels = 6, .maxFrames = 127}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error) ||
            !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1 &&
                       loaded.pipeline->latencyFrames() == 0,
                   "Bass Management IIR must execute without asset warnings or latency")) return false;
        auto energies = std::array<double, 6>{};
        auto inputEnergy = 0.0;
        for (auto start = 0u; start < 48001u;) {
          const auto count = std::min(127u, 48001u - start);
          auto audio = std::vector<float>(6u * count);
          for (auto ch = 0u; ch < 6u; ++ch)
            for (auto i = 0u; i < count; ++i)
              audio[ch * count + i] = static_cast<float>(0.2 *
                  std::sin(2 * std::numbers::pi * frequency * (start + i) / 48000.0));
          const auto input = audio;
          if (!check(loaded.pipeline->process(audio, 6, count, start / 48000.0) ==
                         pipetune::ProcessStatus::ok, "Bass Management IIR PCM failed")) return false;
          if (start >= 24000u) {
            const auto headroom = std::pow(10.0F, -6.0F / 20.0F);
            const auto lfeGain = std::pow(10.0F, -3.0F / 20.0F);
            for (auto i = 0u; i < count; ++i) {
              inputEnergy += input[i] * input[i];
              if (!check(std::abs(audio[2u * count + i] + audio[3u * count + i] -
                                 input[count + i] * headroom * lfeGain) < 2.0e-6F,
                         "opposite sub routes must cancel managed bass and retain only routed LFE") ||
                  !check(std::abs(audio[4u * count + i] - input[4u * count + i] * headroom) < 2.0e-6F &&
                             audio[count + i] == 0 && audio[5u * count + i] == 0,
                         "Full Range must retain headroom while LFE source and Unused outputs are silent")) return false;
              for (auto ch = 0u; ch < 6; ++ch) energies[ch] += audio[ch * count + i] * audio[ch * count + i];
            }
          }
          start += count;
        }
        const auto main = std::sqrt(energies[0] / inputEnergy);
        const auto sub = std::sqrt(energies[3] / inputEnergy);
        const auto expectedSub = std::pow(10.0, -12.0 / 20.0) / 2;
        if (!check(frequency == 20 ? main < 0.005 && std::abs(sub - expectedSub) < 0.002 :
                                    main > 0.49 && sub < 0.001,
                   "Managed crossover must split bands with gain, polarity and 1/N sub distribution")) return false;
      }
    }
  }
  return true;
}

static bool testLfeOverlap(const std::filesystem::path &path) {
  for (const auto channels : {4u, 16u}) {
    for (const auto lowpass : {false, true}) {
      const auto mask = 1u << (channels - 1u);
      {
        auto file = std::ofstream(path);
        file << R"({"pipeline":[{"name":"Bass Management","channel":"A","parameters":{"su":)" << mask
             << ",\"ro" << channels - 1u << "\":2,\"rt" << channels - 1u << "\":" << mask
             << ",\"lo\":" << (lowpass ? "true" : "false") << ",\"ls\":48,\"lf\":120}}]}";
      }
      auto loaded = pipetune::loadDspPipeline(path,
          {.sampleRate = 48000, .maxChannels = channels, .maxFrames = 128});
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1,
                 "LFE input may share the sub output at the highest channel")) return false;
      auto energy = 0.0;
      auto reference = 0.0;
      for (auto start = 0u; start < 24000u; start += 128u) {
        auto audio = std::vector<float>(channels * 128u, 0.0F);
        for (auto i = 0u; i < 128u; ++i)
          audio[(channels - 1u) * 128u + i] = static_cast<float>(0.2 *
              std::sin(2 * std::numbers::pi * 1000 * (start + i) / 48000.0));
        const auto input = audio;
        if (!check(loaded.pipeline->process(audio, channels, 128, start / 48000.0) ==
                       pipetune::ProcessStatus::ok, "overlapping LFE/sub processing failed")) return false;
        if (start < 12000u) continue;
        for (auto i = 0u; i < audio.size(); ++i) {
          if (!lowpass && !check(audio[i] == input[i], "overlapping LFE must be sent once, without doubling")) return false;
          energy += audio[i] * audio[i];
          reference += input[i] * input[i];
        }
      }
      if (!check(lowpass ? energy < reference * 1.0e-5 : energy == reference,
                 "optional LFE low-pass must suppress high-frequency content")) return false;
    }
  }
  return true;
}

static bool testNoFilters(const std::filesystem::path &path) {
  for (const auto linear : {false, true}) {
    for (const auto unconfigured : {false, true}) {
      {
        auto file = std::ofstream(path);
        file << R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"ph":")"
             << (linear ? "Linear" : "IIR") << R"(","tp":"8192","hg":-6,)"
             << (unconfigured ? R"("su":0,"ro":[1,2,3,3,1],"rt":[65535],"ri":[4])" :
                                 R"("su":8,"ro":[0,0,0,2],"rt":[0,0,0,8])") << "}}]}";
      }
      auto loaded = pipetune::loadDspPipeline(path,
          {.sampleRate = 48000, .maxChannels = 4, .maxFrames = 127});
      const auto latency = linear ? 4224u : 0u;
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1 &&
                     loaded.pipeline->latencyFrames() == latency,
                 "no-lowpass Bass Management must retain its documented gain/delay without an asset")) return false;
      for (auto start = 0u; start < latency + 2048u; start += 127u) {
        auto audio = std::vector<float>(4u * 127u, 0.0F);
        if (start == 0) for (auto ch = 0u; ch < 4; ++ch) audio[ch * 127u + 7u] = 0.25F;
        if (!check(loaded.pipeline->process(audio, 4, 127, start / 48000.0) == pipetune::ProcessStatus::ok,
                   "no-lowpass Bass Management PCM failed")) return false;
        for (auto ch = 0u; ch < 4; ++ch)
          for (auto i = 0u; i < 127u; ++i) {
            const auto expected = start + i == latency + 7u ? 0.25F * std::pow(10.0F, -6.0F / 20.0F) : 0.0F;
            if (!check(std::abs(audio[ch * 127u + i] - expected) < 2.0e-6F,
                       "no-lowpass Bass Management must keep every channel aligned and apply headroom")) return false;
          }
      }
    }
  }
  return true;
}

static bool testSettingsValidation(const std::filesystem::path &path) {
  const auto parameters = std::array<std::string_view, 10>{
      R"({"su":16})", R"({"su":8,"ro":[0,0,0,3,1]})",
      R"({"su":8,"ro":[0,0,0,3,2]})", R"({"su":8,"ro":[0,0,0,3],"rt4":8})",
      R"({"su":8})", R"({"su":8,"ro":[1,0,0,3]})",
      R"({"su":8,"ro":[1,0,0,3],"rt":[4]})", R"({"su":8,"ro":[1,0,0,3],"rt":[8],"ri":[4]})",
      R"({"sl":[36]})", R"({"ls":72})"};
  for (const auto settings : parameters) {
    {
      auto file = std::ofstream(path);
      file << "{\"pipeline\":[{\"name\":\"Bass Management\",\"channel\":\"All\",\"parameters\":" << settings << "}]}";
    }
    const auto loaded = pipetune::loadDspPipeline(path,
        {.sampleRate = 48000, .maxChannels = 4, .maxFrames = 128});
    if (!check(loaded.pipeline == nullptr && loaded.warnings.empty() &&
                   loaded.error.find("Bass Management") != std::string::npos,
               "invalid Bass Management routes or slopes must fail with a named error")) return false;
  }
  for (const auto selection : {"", ",\"channel\":\"34\"", ",\"channel\":\"3\""}) {
    {
      auto file = std::ofstream(path);
      file << "{\"pipeline\":[{\"name\":\"Bass Management\"" << selection << "}]}";
    }
    const auto loaded = pipetune::loadDspPipeline(path,
        {.sampleRate = 48000, .maxChannels = 4, .maxFrames = 128});
    if (!check(loaded.pipeline == nullptr && loaded.error.find("Bass Management") != std::string::npos &&
                   loaded.error.find("All") != std::string::npos,
               "Bass Management must require an explicit All route")) return false;
  }
  {
    auto file = std::ofstream(path);
    file << R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"ph":"Linear","su":8,"ro":[1,0,0,3],"rt":[8]}},{"name":"Attack Tonal Balance","channel":"All"}]})";
  }
  const auto linear = pipetune::loadDspPipeline(path,
      {.sampleRate = 48000, .maxChannels = 4, .maxFrames = 128});
  if (!check(linear.pipeline != nullptr, linear.error) ||
      !check(linear.warnings.empty() && linear.pipeline->latencyFrames() == 13440,
             "Linear Bass Management FIR and downstream Attack delays must add")) return false;
  for (auto start = 0u; start < 196608u; start += 128u) {
    auto audio = std::vector<float>(512, 0.0F);
    if (!check(linear.pipeline->process(audio, 4, 128, start / 48000.0) == pipetune::ProcessStatus::ok,
               "Linear FIR preparation must process silence")) return false;
  }
  auto subEnergy = 0.0;
  for (auto start = 0u; start < 32768u; start += 128u) {
    auto audio = std::vector<float>(512, 0.0F);
    if (start == 0) audio[17] = 1.0F;
    if (!check(linear.pipeline->process(audio, 4, 128, (196608.0 + start) / 48000.0) == pipetune::ProcessStatus::ok,
               "Linear Bass Management must process an impulse after preparation")) return false;
    for (auto i = 0u; i < 128u; ++i) {
      subEnergy += audio[384u + i] * audio[384u + i];
      if (!check(std::abs(audio[i] + audio[384u + i] - (start + i == 13457u ? 1.0F : 0.0F)) < 2.0e-5F,
                 "Linear high and low outputs must reconstruct the delayed input")) return false;
    }
  }
  return check(subEnergy > 1.0e-5, "Linear FIR must become active and send filtered audio to the sub");
}

static bool testStereoSubsAndBypass(const std::filesystem::path &path) {
  {
    auto file = std::ofstream(path);
    file << R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"su":12,"ro":[1,1,3,3],"rt":[12,12]}}]})";
  }
  auto loaded = pipetune::loadDspPipeline(path, {.sampleRate = 48000, .maxChannels = 4, .maxFrames = 128});
  if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
  auto audio = std::vector<float>(512);
  for (auto start = 0u; start < 24000u; start += 128u) {
    for (auto ch = 0u; ch < 4; ++ch)
      std::fill_n(audio.data() + ch * 128u, 128u, 0.2F * (ch + 1));
    if (!check(loaded.pipeline->process(audio, 4, 128, start / 48000.0) == pipetune::ProcessStatus::ok,
               "stereo mains and two subs must process")) return false;
  }
  for (auto ch = 0u; ch < 4; ++ch)
    for (auto i = 0u; i < 128u; ++i)
      if (!check(std::abs(audio[ch * 128u + i] - (ch < 2 ? 0.0F : 0.3F)) < 1.0e-5F,
                 "stereo DC must go equally to two subs, discarding Unused input channels")) return false;
  auto bypass = pipetune::createBypassDspPipeline({.sampleRate = 48000, .maxChannels = 4, .maxFrames = 128});
  if (!check(bypass.pipeline != nullptr, bypass.error)) return false;
  loaded.pipeline = std::move(bypass.pipeline);
  for (auto ch = 0u; ch < 4; ++ch)
    std::fill_n(audio.data() + ch * 128u, 128u, 0.2F * (ch + 1));
  const auto original = audio;
  return check(loaded.pipeline->process(audio, 4, 128, 1.0) == pipetune::ProcessStatus::ok && audio == original,
               "host bypass must restore the original mains and sub-channel wiring");
}

int main() {
  const auto path = std::filesystem::temp_directory_path() /
      ("pipetune-bass-management-" + std::to_string(getpid()) + ".effetune_preset");
  const auto passed = testIirRouting(path) && testLfeOverlap(path) &&
      testNoFilters(path) && testSettingsValidation(path) && testStereoSubsAndBypass(path);
  std::filesystem::remove(path);
  return passed ? 0 : 1;
}
