/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipetune/dsp_pipeline.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

static bool check(bool condition, const std::string &message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static bool testInputAlignment(const std::filesystem::path &path) {
  struct Case { const char *name; unsigned width; const char *selection; bool send; };
  const auto cases = std::vector<Case>{
      {"Matrix", 2, "All", false}, {"Matrix", 4, "All", false},
      {"Matrix", 4, "34", false}, {"Matrix", 2, "All", true}, {"Matrix", 4, "34", true},
      {"Spatial Mapper", 2, "All", false}, {"Spatial Mapper", 4, "34", false},
      {"Spatial Mapper", 2, "All", true},
      {"Bass Management", 6, "IIR", false}, {"Bass Management", 6, "Linear", false}};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto &test : cases) {
      const auto pair = std::string_view(test.selection) == "34";
      const auto spatial = std::string_view(test.name) == "Spatial Mapper";
      const auto bass = std::string_view(test.name) == "Bass Management";
      const auto first = pair ? 2u : 0u;
      const auto selected = pair ? 2u : test.width;
      const auto mixerDelay = spatial ? 2560u : bass && std::string_view(test.selection) == "Linear" ? 4224u : 0u;
      const auto latency = 64u + mixerDelay;
      auto parameters = std::string{};
      if (spatial) {
        auto matrix = std::string{};
        for (auto index = 0u; index < 256u; ++index) {
          if (index != 0u) matrix += ',';
          matrix += index < 2u ? "1" : "0";
        }
        parameters = "\"ic\":2,\"ep\":false,\"dm\":[" + matrix + "],\"fm\":[" + matrix + "],\"rm\":[" + matrix + "]";
      } else if (bass) {
        parameters = "\"ph\":\"" + std::string(test.selection) +
            "\",\"tp\":\"8192\",\"su\":12,\"ro\":[0,2,3,3,0,3],\"rt\":[0,12],\"lo\":false";
      } else {
        parameters = "\"mx\":\"";
        for (auto ch = 0u; ch < selected; ++ch) parameters += std::to_string(ch) + "0";
        parameters += '"';
      }
      {
        auto file = std::ofstream(path);
        file << R"({"pipeline":[{"name":"Saturation","parameters":{"os":2,"mx":0,"gn":0},"channel":")"
             << first + 1u << R"("},{"name":")" << test.name << "\",\"channel\":\"" << (bass ? "All" : test.selection)
             << "\",\"parameters\":{" << parameters << '}';
        if (test.send) file << R"(,"inputBus":0,"outputBus":1)";
        file << '}';
        if (test.send) {
          // A send must align only its working copy. Delay the previously
          // undelayed source channel afterwards; this must add 64, not 128.
          file << R"(,{"name":"Saturation","parameters":{"os":2,"mx":0,"gn":0},"channel":")"
               << first + 2u << R"("},{"name":"Volume","channel":"All","inputBus":1,"outputBus":0})";
        }
        file << "]}";
      }
      auto loaded = pipetune::loadDspPipeline(path, {48000.0F, test.width, 257u}, backend);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.pipeline->latencyFrames() == latency,
                 std::string(test.name) + " selected input alignment must not double-count source delay")) return false;
      // Reset must retain routes even before their first pending application.
      if (loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
      auto amplitudes = std::vector<float>(test.width);
      for (auto ch = 0u; ch < test.width; ++ch) amplitudes[ch] = (ch + 1u) * 0.05F;
      if (bass) amplitudes = {0.05F, 0.0F, 0.05F, 0.05F, 0.25F, 0.0F};
      else {
        auto sum = 0.0F;
        for (auto ch = 0u; ch < selected; ++ch) sum += amplitudes[first + ch];
        for (auto ch = 0u; ch < selected; ++ch)
          amplitudes[first + ch] = (test.send ? amplitudes[first + ch] : 0.0F) + (ch == 0u ? sum : 0.0F);
      }
      for (auto repeat = 0u; repeat < 2u; ++repeat) {
        if (repeat != 0u && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
        for (auto start = 0u; start < latency + 1024u;) {
          const auto count = std::min(repeat == 0u ? 257u : 63u, latency + 1024u - start);
          auto audio = std::vector<float>(test.width * count, 0.0F);
          if (start <= 173u && start + count > 173u)
            for (auto ch = 0u; ch < test.width; ++ch) audio[ch * count + 173u - start] = (ch + 1u) * 0.05F;
          if (loaded.pipeline->process(audio, test.width, count, start / 48000.0) != pipetune::ProcessStatus::ok) return false;
          for (auto ch = 0u; ch < test.width; ++ch)
            for (auto i = 0u; i < count; ++i) {
              const auto expected = start + i == latency + 173u ? amplitudes[ch] : 0.0F;
              if (!check(std::isfinite(audio[ch * count + i]) && std::abs(audio[ch * count + i] - expected) < 3.0e-5F,
                         std::string(test.name) + " must align before mixing and retain routes/history after reset: repeat=" +
                         std::to_string(repeat) + " frame=" + std::to_string(start + i) + " ch=" + std::to_string(ch) +
                         " actual=" + std::to_string(audio[ch * count + i]) + " expected=" + std::to_string(expected))) return false;
            }
          start += count;
        }
      }
    }
  }
  return true;
}

int main() {
  const auto path = std::filesystem::temp_directory_path() /
      ("pipetune-input-alignment-" + std::to_string(getpid()) + ".effetune_preset");
  const auto passed = testInputAlignment(path);
  std::filesystem::remove(path);
  return passed ? 0 : 1;
}
