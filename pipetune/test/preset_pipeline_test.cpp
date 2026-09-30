/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/dsp_pipeline.h"
#include "pipetune/dsp_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

static bool fail(std::string_view message) {
  std::cerr << message << '\n';
  return false;
}

static bool check(bool condition, std::string_view message) {
  return condition ? true : fail(message);
}

static std::filesystem::path writePreset(const std::filesystem::path &directory,
                                         std::string_view name, std::string_view json) {
  const auto path = directory / name;
  auto stream = std::ofstream(path, std::ios::binary);
  stream.write(json.data(), static_cast<std::streamsize>(json.size()));
  return path;
}

static bool approximately(float actual, float expected, float tolerance = 1.0e-6F) {
  return std::abs(actual - expected) <= tolerance;
}

static bool testBypassPipeline() {
  const auto result = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(result.pipeline != nullptr, result.error) ||
      !check(result.pipeline->sampleRate() == 48000.0F,
             "bypass pipeline must report its prepared sample rate") ||
      !check(result.pipeline->maxChannels() == 2,
             "bypass pipeline must report its prepared channel count") ||
      !check(result.pipeline->maxFrames() == 64,
             "bypass pipeline must report its prepared frame count") ||
      !check(result.pipeline->activePluginCount() == 0,
             "bypass pipeline must not contain native DSP nodes") ||
      !check(result.pipeline->latencyFrames() == 0,
             "bypass pipeline must not add latency")) {
    return false;
  }

  auto samples =
      std::vector<float>{0.75F, -0.25F, 0.125F, -0.5F, 0.25F, -0.125F};
  const auto original = samples;
  if (!check(result.pipeline->process(samples, 2, 3, 1.5) ==
                 pipetune::ProcessStatus::ok,
             "bypass pipeline processing failed") ||
      !check(samples == original,
             "bypass pipeline must leave every PCM sample unchanged")) {
    return false;
  }

  auto invalid = std::vector<float>{0.5F};
  return check(result.pipeline->process(invalid, 2, 1, 1.5) ==
                   pipetune::ProcessStatus::invalidBuffer,
               "bypass pipeline must retain runtime buffer validation");
}

static bool testCanonicalPreset(const std::filesystem::path &directory) {
  const auto path = writePreset(
      directory, "canonical.effetune_preset",
      R"json({
        "name": "Native pipeline",
        "pipeline": [
          {"name":"Section","enabled":false,"parameters":{}},
          {"name":"Volume","enabled":true,"parameters":{"vl":24},"channel":"A"},
          {"name":"Section","enabled":true,"parameters":{}},
          {"name":"Future DSP","enabled":true,"parameters":{}},
          {"name":"IR Reverb","enabled":true,"parameters":{}},
          {"name":"Room EQ","enabled":true,"parameters":{}},
          {"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}
        ],
        "timestamp": 1
      })json");
  const auto result =
      pipetune::loadDspPipeline(path, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(result.pipeline != nullptr, result.error)) {
    return false;
  }
  if (!check(result.warnings.size() == 3, "canonical preset must report three skipped DSPs") ||
      !check(result.pipeline->activePluginCount() == 1,
             "disabled sections must omit their DSP nodes")) {
    return false;
  }

  auto samples = std::vector<float>{1.0F, -0.5F, 0.25F, -1.0F, 0.5F, -0.25F};
  if (!check(result.pipeline->process(samples, 2, 3, 0.0) == pipetune::ProcessStatus::ok,
             "canonical preset processing failed")) {
    return false;
  }
  const auto gain = std::pow(10.0F, -6.0F / 20.0F);
  const auto expected = std::vector<float>{gain, -0.5F * gain, 0.25F * gain,
                                           -gain, 0.5F * gain, -0.25F * gain};
  for (std::size_t index = 0; index < samples.size(); ++index) {
    if (!check(approximately(samples[index], expected[index]),
               "canonical preset PCM output differs from EffeTune DSP output")) {
      return false;
    }
  }
  return true;
}

static bool testLegacyPreset(const std::filesystem::path &directory) {
  const auto path = writePreset(
      directory, "legacy.effetune_preset",
      R"json({"plugins":[{"nm":"Polarity Inversion","en":true,"ch":"A"}]})json");
  const auto result =
      pipetune::loadDspPipeline(path, {.sampleRate = 44100.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(result.pipeline != nullptr, result.error)) {
    return false;
  }
  auto samples = std::vector<float>{0.75F, -0.25F};
  if (!check(result.pipeline->process(samples, 1, 2, 1.0) == pipetune::ProcessStatus::ok,
             "legacy preset processing failed")) {
    return false;
  }
  return check(samples == std::vector<float>({-0.75F, 0.25F}),
               "legacy short-form parameters must be applied");
}

static bool containsWarning(const std::vector<pipetune::PipelineWarning> &warnings,
                            std::string_view expected) {
  return std::ranges::any_of(warnings, [expected](const auto &warning) {
    return warning.pluginName == expected;
  });
}

static bool testEffeTune26Pipeline(const std::filesystem::path &directory) {
  const auto path = writePreset(
      directory, "effetune-2.6.effetune_preset",
      R"json({
        "pipeline": [
          {"name":"SBC Codec Simulator","enabled":true,"parameters":{"bp":35}},
          {"name":"Cassette Artifacts","enabled":true,"parameters":{"dg":"Consumer"}},
          {"name":"G.726 Simulator","enabled":true,"parameters":{"br":"32"}},
          {"name":"GSM-FR Simulator","enabled":true,"parameters":{"tc":1}},
          {"name":"MP3 Codec Simulator","enabled":true,"parameters":{"br":"64"}},
          {"name":"Tape Artifacts","enabled":true,"parameters":{"sp":"15"}},
          {"name":"Tube Simulator","enabled":true,"parameters":{"tp":"12AX7"}},
          {"name":"AM Radio Simulator","enabled":true,"parameters":{"rd":true}},
          {"name":"FM Radio Simulator","enabled":true,"parameters":{"rd":true}},
          {"name":"SW Radio Simulator","enabled":true,"parameters":{"rd":true,"mo":"USB","bf":125}},
          {"name":"Auto Filter","enabled":true,"parameters":{}},
          {"name":"Auto Pan","enabled":true,"parameters":{}},
          {"name":"Chorus","enabled":true,"parameters":{}},
          {"name":"Frequency Shifter","enabled":true,"parameters":{}},
          {"name":"Phaser","enabled":true,"parameters":{}},
          {"name":"Pitch Shifter HQ","enabled":true,"parameters":{}},
          {"name":"Rotary Speaker","enabled":true,"parameters":{}},
          {"name":"Bandwidth Extender","enabled":true,"parameters":{}},
          {"name":"Phase Select EQ","enabled":true,"parameters":{}},
          {"name":"MD Simulator","enabled":true,"parameters":{"md":"LP2 (132 kbps)"}},
          {"name":"FIR Crossover","enabled":true,"parameters":{}},
          {"name":"5Band FIR PEQ","enabled":true,"parameters":{}},
          {"name":"Group Delay EQ","enabled":true,"parameters":{}},
          {"name":"Group Delay PEQ","enabled":true,"parameters":{}},
          {"name":"Room EQ","enabled":true,"parameters":{}},
          {"name":"IR Reverb","enabled":true,"parameters":{}}
        ]
      })json");
  const auto result = pipetune::loadDspPipeline(
      path,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(result.pipeline != nullptr, result.error) ||
      !check(result.pipeline->activePluginCount() == 24,
             "EffeTune generated-asset DSP nodes must become active") ||
      !check(result.warnings.size() == 2,
             "only unresolved asset DSP nodes must be omitted") ||
      !check(containsWarning(result.warnings, "Room EQ") &&
                 containsWarning(result.warnings, "IR Reverb"),
             "every omitted asset-dependent DSP must be identified")) {
    return false;
  }

  auto samples = std::vector<float>(128u, 0.0F);
  return check(result.pipeline->process(samples, 2, 64, 0.0) ==
                   pipetune::ProcessStatus::ok,
               "EffeTune 2.6 DSP nodes must process audio") &&
         check(std::ranges::all_of(samples, [](float value) {
                 return std::isfinite(value);
               }),
               "EffeTune 2.6 DSP output must remain finite");
}

static bool testGeneratedAssetDsp(const std::filesystem::path &directory) {
  struct GeneratedAssetCase {
    std::string_view filename;
    std::string_view plugin;
    std::string_view parameters;
    std::uint32_t channels;
    std::uint32_t expectedLatency;
    bool expectAdditionalChannelOutput;
    bool expectGainIncrease;
  };
  static constexpr std::array cases = {
      GeneratedAssetCase{
          "fir-crossover.effetune_preset", "FIR Crossover",
          R"json({"lt":"0","bc":4,"pm":"min","tp":8192,"f1":500,"s1":-48,"f2":1500,"s2":-72,"f3":5000,"s3":-96})json",
          16u, 0u, true, false},
      GeneratedAssetCase{
          "five-band-fir-peq.effetune_preset", "5Band FIR PEQ",
          R"json({"lt":"0","pm":"min","tp":8192,"f2":1000,"g2":6,"q2":1,"t2":"pk","e2":true})json",
          2u, 0u, false, true},
      GeneratedAssetCase{
          "group-delay-eq.effetune_preset", "Group Delay EQ",
          R"json({"lt":"0","tp":4096,"d0":-1,"d7":2,"d14":1})json",
          2u, 2048u, false, false},
      GeneratedAssetCase{
          "group-delay-peq.effetune_preset", "Group Delay PEQ",
          R"json({"lt":"0","tp":4096,"t0":"pk","f0":1000,"d0":2,"q0":1,"e0":true})json",
          2u, 2048u, false, false}};

  for (const auto &testCase : cases) {
    const auto preset =
        "{\"pipeline\":[{\"name\":\"" + std::string(testCase.plugin) +
        "\",\"enabled\":true,\"channel\":\"A\",\"parameters\":" +
        std::string(testCase.parameters) + "}]}";
    const auto path = writePreset(directory, testCase.filename, preset);
    const auto result = pipetune::loadDspPipeline(
        path,
        {.sampleRate = 48000.0F,
         .maxChannels = testCase.channels,
         .maxFrames = 128u});
    if (!check(result.pipeline != nullptr, result.error) ||
        !check(result.pipeline->activePluginCount() == 1u,
               "generated-asset DSP must become active") ||
        !check(result.warnings.empty(),
               "generated-asset DSP must not be reported as omitted") ||
        !check(result.pipeline->latencyFrames() == testCase.expectedLatency,
               "generated-asset DSP must report its designed latency")) {
      return false;
    }

    auto inputEnergy = 0.0;
    auto outputEnergy = 0.0;
    auto additionalChannelEnergy = 0.0;
    for (auto block = std::uint32_t{0}; block < 48u; ++block) {
      auto samples = std::vector<float>(testCase.channels * 128u, 0.0F);
      for (auto frame = std::uint32_t{0}; frame < 128u; ++frame) {
        const auto sample = static_cast<float>(
            std::sin(2.0 * std::numbers::pi * 1000.0 *
                     static_cast<double>(block * 128u + frame) / 48000.0));
        samples[frame] = sample;
        samples[128u + frame] = sample * 0.5F;
      }
      if (!check(result.pipeline->process(samples, testCase.channels, 128u,
                                          block * 128.0 / 48000.0) ==
                     pipetune::ProcessStatus::ok,
                 "generated-asset DSP must process audio") ||
          !check(std::ranges::all_of(samples, [](float value) {
                   return std::isfinite(value);
                 }),
                 "generated-asset DSP output must remain finite")) {
        return false;
      }
      if (block >= 32u) {
        for (auto frame = std::uint32_t{0}; frame < 128u; ++frame) {
          const auto sample = std::sin(
              2.0 * std::numbers::pi * 1000.0 *
              static_cast<double>(block * 128u + frame) / 48000.0);
          inputEnergy += sample * sample * 1.25;
        }
        for (const auto sample : samples) {
          outputEnergy += static_cast<double>(sample) * sample;
        }
        for (auto channel = std::uint32_t{2}; channel < testCase.channels;
             ++channel) {
          for (auto frame = std::uint32_t{0}; frame < 128u; ++frame) {
            const auto sample = samples[channel * 128u + frame];
            additionalChannelEnergy +=
                static_cast<double>(sample) * sample;
          }
        }
      }
    }
    if (!check(outputEnergy > 0.01,
               "generated-asset DSP must produce audible output after preparation") ||
        (testCase.expectAdditionalChannelOutput &&
         !check(additionalChannelEnergy > inputEnergy * 0.001,
                "FIR Crossover must route filtered audio to additional output channels")) ||
        (testCase.expectGainIncrease &&
         !check(outputEnergy > inputEnergy * 1.5,
                "5Band FIR PEQ must apply the requested 1 kHz gain"))) {
      return false;
    }
  }
  return true;
}

static bool testFirCrossoverRouting(const std::filesystem::path &directory) {
  const auto discovered = pipetune::discoverDspBackends();
  auto backends = std::vector<std::shared_ptr<const pipetune::DspBackend>>{
      discovered.scalar.backend};
  for (const auto &variant : discovered.simdVariants) {
    if (variant.backend != nullptr) backends.push_back(variant.backend);
  }
  for (const auto &backend : backends) {
    if (!check(backend != nullptr, "FIR tests require the scalar backend")) return false;
    for (const auto channels : {2u, 4u, 6u, 8u, 16u}) {
      for (const auto selection : {"", "34", "A"}) {
        if (channels == 2u && std::string_view(selection) == "34") continue;
        if (channels != 2u && std::string_view(selection) == "A") continue;
        for (const auto transfer : {false, true}) {
          const auto routing = transfer ? ",\"inputBus\":0,\"outputBus\":1" : "";
          const auto returnNode = transfer ?
              ",{\"name\":\"Volume\",\"channel\":\"A\",\"parameters\":{},\"inputBus\":1,\"outputBus\":0}" : "";
          const auto path = writePreset(directory, "fir-pair.effetune_preset",
              "{\"pipeline\":[{\"name\":\"FIR Crossover\",\"channel\":\"" +
              std::string(selection) + "\",\"parameters\":{\"pm\":\"lin\",\"tp\":8192,\"lt\":\"128\",\"bc\":4}" + routing + "}" + returnNode + "]}");
          const auto result = pipetune::loadDspPipeline(path,
              {.sampleRate = 48000.0F, .maxChannels = channels, .maxFrames = 64}, backend);
          if (!check(result.pipeline != nullptr, result.error) ||
              !check(result.warnings.empty(), "stereo FIR must not warn") ||
              !check(result.pipeline->activePluginCount() == (transfer ? 2u : 1u),
                     "stereo FIR must retain its routing node") ||
              !check(result.pipeline->latencyFrames() == 0, "stereo FIR must have zero latency")) return false;
          auto samples = std::vector<float>(channels * 64u);
          for (auto index = std::size_t{0}; index < samples.size(); ++index)
            samples[index] = static_cast<float>(static_cast<int>(index % 19u) - 9) / 32.0F;
          const auto original = samples;
          if (!check(result.pipeline->process(samples, channels, 64, 0.0) == pipetune::ProcessStatus::ok,
                     "stereo FIR routing must process audio")) return false;
          const auto first = std::string_view(selection) == "34" ? 2u : 0u;
          for (auto channel = 0u; channel < channels; ++channel) {
            const auto gain = transfer && channel >= first && channel < first + 2u ? 2.0F : 1.0F;
            for (auto frame = 0u; frame < 64u; ++frame)
              if (!check(samples[channel * 64u + frame] == gain * original[channel * 64u + frame],
                         "stereo FIR must preserve PCM and the selected bus transfer")) return false;
          }
        }
      }
    }
    for (const auto channels : {1u, 3u, 4u}) {
      const auto path = writePreset(directory, "fir-invalid-width.effetune_preset",
          "{\"pipeline\":[{\"name\":\"FIR Crossover\",\"channel\":\"" +
          std::string(channels == 4u ? "1" : "A") + "\",\"parameters\":{}}]}");
      const auto result = pipetune::loadDspPipeline(path,
          {.sampleRate = 48000.0F, .maxChannels = channels, .maxFrames = 64}, backend);
      if (!check(result.pipeline != nullptr, result.error) ||
          !check(result.warnings.size() == 1 && containsWarning(result.warnings, "FIR Crossover"),
                 "mono and odd FIR widths must warn") ||
          !check(result.pipeline->activePluginCount() == 0 && result.pipeline->latencyFrames() == 0,
                 "unsupported FIR widths must omit the node")) return false;
    }
  }
  return true;
}

static bool testFirCrossoverBands(const std::filesystem::path &directory) {
  const auto path = writePreset(directory, "fir-bands.effetune_preset", R"json(
    {"pipeline":[{"name":"FIR Crossover","channel":"A","parameters":
      {"pm":"lin","tp":8192,"lt":"128","bc":4,"f1":500,"f2":2000,"f3":8000,"s1":-96,"s2":-96,"s3":-96}}]})json");
  const auto source = pipetune::loadDspPipeline(path,
      {.sampleRate = 48000.0F, .maxChannels = 4, .maxFrames = 128});
  if (!check(source.pipeline != nullptr, source.error)) return false;
  std::filesystem::remove(path);
  const auto discovered = pipetune::discoverDspBackends();
  auto backends = std::vector<std::shared_ptr<const pipetune::DspBackend>>{discovered.scalar.backend};
  for (const auto &variant : discovered.simdVariants) {
    if (variant.backend != nullptr) backends.push_back(variant.backend);
  }
  for (const auto &backend : backends) {
    const auto stereo = pipetune::rebuildDspPipeline(*source.pipeline,
        {.sampleRate = 96000.0F, .maxChannels = 2, .maxFrames = 128}, backend);
    if (!check(stereo.pipeline != nullptr, stereo.error) ||
        !check(stereo.warnings.empty() && stereo.pipeline->latencyFrames() == 0,
               "rebuild to stereo must discard crossover latency")) return false;
    auto impulse = std::vector<float>(256u, 0.0F);
    impulse[0] = 1.0F;
    impulse[128] = -0.5F;
    const auto expected = impulse;
    if (!check(stereo.pipeline->process(impulse, 2, 128, 0.0) == pipetune::ProcessStatus::ok && impulse == expected,
               "rebuilt stereo FIR must pass an impulse unchanged")) return false;
    for (const auto channels : {4u, 6u, 8u, 16u}) {
      const auto rate = channels == 6u ? 96000.0F : 48000.0F;
      const auto bands = std::min(channels / 2u, 4u);
      const auto frequencies = std::array{100.0, 1000.0, 4000.0, 16000.0};
      for (auto band = 0u; band < bands; ++band) {
        const auto result = pipetune::rebuildDspPipeline(*stereo.pipeline,
            {.sampleRate = rate, .maxChannels = channels, .maxFrames = 128}, backend);
        if (!check(result.pipeline != nullptr, result.error) ||
            !check(result.warnings.empty() && result.pipeline->activePluginCount() == 1,
                   "multichannel FIR must regenerate its asset after stereo rebuild") ||
            !check(result.pipeline->latencyFrames() == 4224u,
                   "linear FIR must report head block plus filter delay")) return false;
        auto energy = std::array<double, 16>{};
        for (auto block = 0u; block < 160u; ++block) {
          auto samples = std::vector<float>(channels * 128u, 0.25F);
          for (auto frame = 0u; frame < 128u; ++frame) {
            const auto value = static_cast<float>(std::sin(2.0 * std::numbers::pi * frequencies[band] *
                (block * 128u + frame) / rate));
            samples[frame] = value;
            samples[128u + frame] = value * 0.5F;
          }
          if (!check(result.pipeline->process(samples, channels, 128, block * 128.0 / rate) == pipetune::ProcessStatus::ok,
                     "multichannel FIR must process frequency probes")) return false;
          if (block >= 128u) {
            for (auto channel = 0u; channel < channels; ++channel)
              for (auto frame = 0u; frame < 128u; ++frame)
                energy[channel] += static_cast<double>(samples[channel * 128u + frame]) * samples[channel * 128u + frame];
          }
        }
        if (!check(energy[band * 2u] > 100.0, "FIR must retain the requested frequency in its band") ||
            !check(std::abs(energy[band * 2u + 1u] / energy[band * 2u] - 0.25) < 0.001,
                   "FIR must retain independent left and right gains")) return false;
        for (auto channel = 0u; channel < channels; ++channel) {
          if (channel / 2u != band &&
              !check(energy[channel] < energy[band * 2u] * 0.01,
                     "FIR must reject other bands and clear unused output channels")) return false;
        }
      }
    }
  }
  // Wait in sample frames for asset preparation before measuring the impulse.
  for (auto block = 0u; block < 200u; ++block) {
    auto silence = std::vector<float>(512u, 0.0F);
    if (!check(source.pipeline->process(silence, 4, 128, block * 128.0 / 48000.0) == pipetune::ProcessStatus::ok,
               "FIR impulse preparation must process silence")) return false;
  }
  for (auto block = 0u; block < 64u; ++block) {
    auto impulse = std::vector<float>(512u, 0.0F);
    if (block == 0u) { impulse[0] = 1.0F; impulse[128] = -0.5F; }
    if (!check(source.pipeline->process(impulse, 4, 128, (200u + block) * 128.0 / 48000.0) == pipetune::ProcessStatus::ok,
               "FIR impulse must process")) return false;
    for (auto frame = 0u; frame < 128u; ++frame) {
      const auto expected = block * 128u + frame == 4224u ? 1.0F : 0.0F;
      if (!check(approximately(impulse[frame] + impulse[256u + frame], expected, 1.0e-4F) &&
                 approximately(impulse[128u + frame] + impulse[384u + frame], expected * -0.5F, 1.0e-4F),
                 "linear FIR bands must reconstruct the input impulse at the reported delay")) return false;
    }
  }
  return true;
}

static bool testTubeSimulator25Models(const std::filesystem::path &directory) {
  struct TubeModelCase {
    std::string_view name;
    std::string_view parameters;
  };
  static constexpr std::array cases = {
      TubeModelCase{"6l6gc", R"json({"os":"Power","pt":"6L6GC"})json"},
      TubeModelCase{"kt88", R"json({"os":"Power","pt":"KT88"})json"},
      TubeModelCase{
          "300b",
          R"json({"tp":"Bypass","os":"SingleEnded","sd":"300B"})json"},
      TubeModelCase{
          "2a3",
          R"json({"os":"SingleEnded","sd":"2A3","sb":350,"sr":900,"sp":"5.0"})json"}};

  for (const auto &testCase : cases) {
    const auto preset =
        "{\"pipeline\":[{\"name\":\"Tube Simulator\",\"enabled\":true,"
        "\"parameters\":" +
        std::string(testCase.parameters) + "}]}";
    const auto path = writePreset(
        directory,
        "tube-simulator-" + std::string(testCase.name) +
            ".effetune_preset",
        preset);
    const auto result = pipetune::loadDspPipeline(
        path,
        {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
    if (!check(result.pipeline != nullptr, result.error) ||
        !check(result.pipeline->activePluginCount() == 1,
               "every EffeTune 2.5 Tube Simulator model must become active")) {
      return false;
    }
  }
  return true;
}

static bool testTubeSimulatorLatency(const std::filesystem::path &directory) {
  const auto path = writePreset(
      directory, "tube-simulator.effetune_preset",
      R"json({"pipeline":[{"name":"Tube Simulator","enabled":true,"parameters":{}}]})json");
  const auto result = pipetune::loadDspPipeline(
      path,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  return check(result.pipeline != nullptr, result.error) &&
         check(result.pipeline->activePluginCount() == 1,
               "Tube Simulator must become active at 48 kHz") &&
         check(result.pipeline->latencyFrames() == 64,
               "Tube Simulator must report its 64-frame processing latency");
}

static bool testChannelLatencyCompensation(
    const std::filesystem::path &directory) {
  static constexpr auto frameCount = std::uint32_t{128};
  const auto path = writePreset(
      directory, "channel-latency-compensation.effetune_preset",
      R"json({"pipeline":[{"name":"Tube Simulator","enabled":true,"channel":"L","parameters":{}}]})json");
  const auto result = pipetune::loadDspPipeline(
      path,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = frameCount});
  if (!check(result.pipeline != nullptr, result.error) ||
      !check(result.pipeline->latencyFrames() == 64,
             "channel-routed Tube Simulator must report pipeline latency")) {
    return false;
  }

  auto samples = std::vector<float>(2u * frameCount, 0.0F);
  samples[frameCount] = 1.0F;
  if (!check(result.pipeline->process(samples, 2u, frameCount, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "channel-latency compensation pipeline must process audio")) {
    return false;
  }
  return check(approximately(samples[frameCount], 0.0F),
               "the zero-latency channel must not precede the DSP channel") &&
         check(approximately(samples[frameCount + 64u], 1.0F),
               "the zero-latency channel must align with pipeline latency");
}

static bool channelsApproximatelyEqual(std::span<const float> samples,
                                       std::uint32_t firstChannel,
                                       std::uint32_t secondChannel,
                                       std::uint32_t frameCount) {
  const auto firstOffset = static_cast<std::size_t>(firstChannel) * frameCount;
  const auto secondOffset = static_cast<std::size_t>(secondChannel) * frameCount;
  for (auto frame = std::uint32_t{0}; frame < frameCount; ++frame) {
    if (!approximately(samples[firstOffset + frame],
                       samples[secondOffset + frame], 2.0e-5F)) {
      return false;
    }
  }
  return true;
}

static bool testBalanceChannelPairs(const std::filesystem::path &directory) {
  static constexpr auto frameCount = std::uint32_t{512};
  const auto run = [&](std::string_view filename, std::string_view plugin,
                       std::string_view parameters) {
    const auto path = writePreset(
        directory, filename,
        "{\"pipeline\":[{\"name\":\"" + std::string(plugin) +
            "\",\"enabled\":true,\"channel\":\"A\",\"parameters\":" +
            std::string(parameters) + "}]}");
    const auto result = pipetune::loadDspPipeline(
        path,
        {.sampleRate = 48000.0F, .maxChannels = 4, .maxFrames = frameCount});
    if (!check(result.pipeline != nullptr, result.error)) {
      return false;
    }
    auto samples = std::vector<float>(4u * frameCount);
    for (auto channel = std::uint32_t{0}; channel < 4u; ++channel) {
      for (auto frame = std::uint32_t{0}; frame < frameCount; ++frame) {
        samples[static_cast<std::size_t>(channel) * frameCount + frame] =
            static_cast<float>(static_cast<int>(frame % 29u) - 14) * 0.025F;
      }
    }
    if (!check(result.pipeline->process(samples, 4u, frameCount, 0.0) ==
                   pipetune::ProcessStatus::ok,
               "balance DSP must process four-channel audio")) {
      return false;
    }
    return check(channelsApproximatelyEqual(samples, 0u, 2u, frameCount) &&
                     channelsApproximatelyEqual(samples, 1u, 3u, frameCount) &&
                     !channelsApproximatelyEqual(samples, 0u, 1u, frameCount),
                 "balance DSP must affect and apply the same rule to every "
                 "channel pair");
  };

  return run("stereo-balance-pairs.effetune_preset", "Stereo Balance",
             R"json({"bl":0.5})json") &&
         run("multiband-balance-pairs.effetune_preset", "Multiband Balance",
             R"json({"bands":[{"balance":100},{"balance":100},{"balance":100},{"balance":100},{"balance":100}]})json");
}

static bool testRawLegacyPipeline(const std::filesystem::path &directory) {
  const auto path = writePreset(
      directory, "raw-legacy.effetune_preset",
      R"json([{"nm":"Volume","en":true,"vl":6,"ch":"A"}])json");
  const auto result =
      pipetune::loadDspPipeline(path, {.sampleRate = 96000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(result.pipeline != nullptr, result.error)) {
    return false;
  }
  auto samples = std::vector<float>{0.25F};
  if (!check(result.pipeline->process(samples, 1, 1, 2.0) == pipetune::ProcessStatus::ok,
             "raw legacy pipeline processing failed")) {
    return false;
  }
  return check(approximately(samples[0], 0.25F * std::pow(10.0F, 6.0F / 20.0F)),
               "raw legacy pipeline parameters must be applied");
}

static bool testRejectedInputs(const std::filesystem::path &directory) {
  const auto wrongExtension =
      writePreset(directory, "wrong.effetune-preset", R"json({"pipeline":[]})json");
  const auto wrongResult = pipetune::loadDspPipeline(
      wrongExtension, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(wrongResult.pipeline == nullptr,
             "the unrequested .effetune-preset extension must be rejected")) {
    return false;
  }

  const auto badBus = writePreset(
      directory, "bad-bus.effetune_preset",
      R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{},"inputBus":5}]})json");
  const auto badBusResult =
      pipetune::loadDspPipeline(badBus, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(badBusResult.pipeline == nullptr, "an active DSP with an invalid bus must fail")) {
    return false;
  }

  const auto malformed =
      writePreset(directory, "malformed.effetune_preset", R"json({"pipeline":[)json");
  const auto malformedResult = pipetune::loadDspPipeline(
      malformed, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(malformedResult.pipeline == nullptr, "malformed JSON must fail")) {
    return false;
  }

  const auto valid =
      writePreset(directory, "valid.effetune_preset", R"json({"pipeline":[]})json");
  const auto invalidRate =
      pipetune::loadDspPipeline(valid, {.sampleRate = 31999.0F, .maxChannels = 2, .maxFrames = 64});
  const auto invalidChannels =
      pipetune::loadDspPipeline(valid, {.sampleRate = 48000.0F, .maxChannels = 17, .maxFrames = 64});
  return check(invalidRate.pipeline == nullptr, "sample rates below 32 kHz must fail") &&
         check(invalidChannels.pipeline == nullptr, "more than sixteen channels must fail");
}

static bool testRuntimeBounds(const std::filesystem::path &directory) {
  const auto path =
      writePreset(directory, "bounds.effetune_preset", R"json({"pipeline":[]})json");
  const auto result =
      pipetune::loadDspPipeline(path, {.sampleRate = 384000.0F, .maxChannels = 16, .maxFrames = 32});
  if (!check(result.pipeline != nullptr, result.error)) {
    return false;
  }
  auto samples = std::vector<float>(64, 0.25F);
  if (!check(result.pipeline->sampleRate() == 384000.0F,
             "pipeline must report its prepared sample rate") ||
      !check(result.pipeline->process(samples, 16, 4, 0.0) == pipetune::ProcessStatus::ok,
             "maximum supported channel/rate configuration must process") ||
      !check(std::ranges::all_of(samples, [](float value) { return value == 0.25F; }),
             "an empty pipeline must be transparent")) {
    return false;
  }
  auto oversized = std::vector<float>(16u * 33u, 0.25F);
  return check(result.pipeline->process(oversized, 16, 33, 0.0) ==
                   pipetune::ProcessStatus::invalidBuffer,
               "processing beyond the prepared frame count must fail safely");
}

static bool testEffeTune27Multichannel(
    const std::filesystem::path &directory) {
  constexpr auto channelCount = std::uint32_t{16};
  constexpr auto frameCount = std::uint32_t{32};
  const auto routedPath = writePreset(
      directory, "effetune-2.7-routes.effetune_preset",
      R"json({"pipeline":[
        {"name":"Polarity Inversion","enabled":true,"parameters":{},"channel":"9"},
        {"name":"Polarity Inversion","enabled":true,"parameters":{},"channel":"1516"}
      ]})json");
  const auto routed = pipetune::loadDspPipeline(
      routedPath,
      {.sampleRate = 48000.0F,
       .maxChannels = channelCount,
       .maxFrames = frameCount});
  if (!check(routed.pipeline != nullptr, routed.error)) {
    return false;
  }
  auto routedSamples =
      std::vector<float>(channelCount * frameCount, 1.0F);
  if (!check(routed.pipeline->process(routedSamples, channelCount, frameCount,
                                      0.0) ==
                 pipetune::ProcessStatus::ok,
             "EffeTune 2.7 channel selections must process")) {
    return false;
  }
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto expected = channel == 8u || channel >= 14u ? -1.0F : 1.0F;
    const auto samples = std::span<const float>(routedSamples).subspan(
        channel * frameCount, frameCount);
    if (!check(std::ranges::all_of(samples, [expected](float sample) {
                 return sample == expected;
               }),
               "EffeTune 2.7 must route channels 9 and 15/16")) {
      return false;
    }
  }

  const auto panelPath = writePreset(
      directory, "effetune-2.7-panel.effetune_preset",
      R"json({"pipeline":[{"name":"MultiChannel Panel","enabled":true,"parameters":{
        "m":[false,false,false,false,false,false,false,false,false,false,false,false,false,false,false,true]
      },"channel":"A"}]})json");
  const auto panel = pipetune::loadDspPipeline(
      panelPath,
      {.sampleRate = 48000.0F,
       .maxChannels = channelCount,
       .maxFrames = frameCount});
  if (!check(panel.pipeline != nullptr, panel.error)) {
    return false;
  }
  auto panelSamples = std::vector<float>(channelCount * frameCount, 1.0F);
  if (!check(panel.pipeline->process(panelSamples, channelCount, frameCount,
                                     0.0) ==
                 pipetune::ProcessStatus::ok,
             "EffeTune 2.7 Multi Channel Panel must process 16 channels")) {
    return false;
  }
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto expected = channel == 15u ? 0.0F : 1.0F;
    const auto samples = std::span<const float>(panelSamples).subspan(
        channel * frameCount, frameCount);
    if (!check(std::ranges::all_of(samples, [expected](float sample) {
                 return sample == expected;
               }),
               "Multi Channel Panel must apply channel 16 parameters")) {
      return false;
    }
  }

  const auto matrixPath = writePreset(
      directory, "effetune-2.7-matrix.effetune_preset",
      R"json({"pipeline":[{"name":"Matrix","enabled":true,"parameters":{"mx":"f0"},"channel":"A"}]})json");
  const auto matrix = pipetune::loadDspPipeline(
      matrixPath,
      {.sampleRate = 48000.0F,
       .maxChannels = channelCount,
       .maxFrames = frameCount});
  if (!check(matrix.pipeline != nullptr, matrix.error)) {
    return false;
  }
  auto matrixSamples = std::vector<float>(channelCount * frameCount, 0.0F);
  std::fill(matrixSamples.begin() + 15u * frameCount, matrixSamples.end(),
            0.25F);
  if (!check(matrix.pipeline->process(matrixSamples, channelCount, frameCount,
                                      0.0) ==
                 pipetune::ProcessStatus::ok,
             "EffeTune 2.7 Matrix must process hexadecimal routes")) {
    return false;
  }
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto expected = channel == 0u ? 0.25F : 0.0F;
    const auto samples = std::span<const float>(matrixSamples).subspan(
        channel * frameCount, frameCount);
    if (!check(std::ranges::all_of(samples, [expected](float sample) {
                 return sample == expected;
               }),
               "Matrix route f0 must send channel 16 to channel 1")) {
      return false;
    }
  }
  return true;
}

static bool testVisualizationPresets(const std::filesystem::path &directory) {
  const auto visualizers = std::array{
      "Level Meter", "Oscilloscope", "Spectrogram", "Spectrum Analyzer", "Stereo Meter", "Note Spectrogram", "Pitch Meter", "Chroma Spiral"};
  auto nodes = std::string{};
  for (auto index = 0u; index < 100u; ++index) {
    if (!nodes.empty()) nodes += ',';
    nodes += "{\"name\":\"" + std::string(visualizers[index % visualizers.size()]) +
        "\",\"parameters\":{\"mn\":{},\"cl\":\"Normal\",\"hq\":true},\"inputBus\":0,\"outputBus\":1}";
  }
  const auto only = writePreset(directory, "visualizers.effetune_preset",
                                "{\"pipeline\":[" + nodes + "]}");
  auto loaded = pipetune::loadDspPipeline(
      only, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(loaded.pipeline != nullptr, loaded.error) ||
      !check(loaded.warnings.empty(), "visualizers must not produce warnings") ||
      !check(loaded.pipeline->activePluginCount() == 0 &&
                 loaded.pipeline->latencyFrames() == 0,
             "visualizers must not consume processing nodes or add latency")) {
    return false;
  }
  auto samples = std::vector<float>{0.5F, -0.25F, 0.125F, -0.5F};
  const auto original = samples;
  if (!check(loaded.pipeline->process(samples, 2, 2, 0.0) == pipetune::ProcessStatus::ok &&
                 samples == original,
             "a visualization-only preset must pass audio without copying other buses")) {
    return false;
  }

  const auto effects = std::string(R"json(
    {"name":"Compressor","parameters":{}},
    {"name":"Auto Leveler","parameters":{}},
    {"name":"Transient Shaper","parameters":{}},
    {"name":"Multiband Transient","parameters":{}},
    {"name":"Power Amp Sag","parameters":{}},
    {"name":"Volume","parameters":{"vl":-6}},
    {"name":"Volume","parameters":{"vl":-12},"inputBus":1,"outputBus":0}
  )json");
  const auto sections = std::string(R"json(
    {"name":"Section","enabled":false},
    {"name":"Mute"},
    {"name":"Section","enabled":true},
  )json");
  const auto mixed = writePreset(directory, "mixed-visualizers.effetune_preset",
      "{\"pipeline\":[" + sections + nodes + "," + effects + "]}");
  const auto reference = writePreset(directory, "audio-effects.effetune_preset",
      "{\"pipeline\":[" + sections + effects + "]}");
  auto actual = pipetune::loadDspPipeline(
      mixed, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  auto expected = pipetune::loadDspPipeline(
      reference, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(actual.pipeline != nullptr, actual.error) ||
      !check(expected.pipeline != nullptr, expected.error) ||
      !check(actual.warnings.empty() && actual.pipeline->activePluginCount() == 7,
             "audio processors with meters must remain active")) {
    return false;
  }
  std::filesystem::remove(mixed);
  const auto backends = pipetune::discoverDspBackends();
  for (const auto rate : {48000.0F, 96000.0F}) {
    const auto backend = rate == 96000.0F && backends.simd.backend != nullptr
        ? backends.simd.backend : backends.scalar.backend;
    actual = pipetune::rebuildDspPipeline(*actual.pipeline,
        {.sampleRate = rate, .maxChannels = 2, .maxFrames = 64}, backend);
    expected = pipetune::rebuildDspPipeline(*expected.pipeline,
        {.sampleRate = rate, .maxChannels = 2, .maxFrames = 64}, backend);
    if (!check(actual.pipeline != nullptr, actual.error) ||
        !check(expected.pipeline != nullptr, expected.error) ||
        !check(actual.warnings.empty() && actual.pipeline->activePluginCount() == 7 &&
                   actual.pipeline->latencyFrames() == expected.pipeline->latencyFrames(),
               "rebuild must preserve only the audio processors")) {
      return false;
    }
    for (auto block = 0u; block < 100u; ++block) {
      auto input = std::vector<float>(128);
      for (auto index = 0u; index < input.size(); ++index) {
        input[index] = 0.2F * std::sin(static_cast<float>(block * 64u + index) * 0.1F);
      }
      auto referenceAudio = input;
      const auto time = static_cast<double>(block * 64u) / rate;
      if (!check(actual.pipeline->process(input, 2, 64, time) == pipetune::ProcessStatus::ok &&
                     expected.pipeline->process(referenceAudio, 2, 64, time) == pipetune::ProcessStatus::ok &&
                     input == referenceAudio,
                 "mixed presets must render exactly like their audio-processing nodes")) {
        return false;
      }
    }
  }
  return true;
}

static bool testSpatialMapper(const std::filesystem::path &directory) {
  struct SpatialCase {
    float rate;
    std::uint32_t channels;
    std::string_view bands;
    std::string_view selection;
    bool mapped;
    bool transfer;
    std::uint32_t latency;
  };
  const auto cases = std::array{
      SpatialCase{44100, 1, "8", "All", false, false, 2560},
      SpatialCase{48000, 2, "16", "", false, false, 2560},
      SpatialCase{96000, 6, "24", "All", false, false, 5120},
      SpatialCase{192000, 12, "32", "All", false, false, 10240},
      SpatialCase{384000, 16, "48", "All", false, false, 20480},
      SpatialCase{48000, 6, "24", "All", true, false, 2560},
      SpatialCase{48000, 12, "48", "All", true, false, 2560},
      SpatialCase{48000, 16, "8", "All", true, false, 2560},
      SpatialCase{48000, 6, "16", "34", false, false, 2560},
      SpatialCase{48000, 6, "32", "3", false, false, 2560},
      SpatialCase{48000, 6, "24", "All", false, true, 2560}};
  const auto discovered = pipetune::discoverDspBackends();
  auto backends = std::vector<std::shared_ptr<const pipetune::DspBackend>>{
      discovered.scalar.backend};
  for (const auto &variant : discovered.simdVariants)
    if (variant.backend != nullptr) backends.push_back(variant.backend);
  for (const auto &backend : backends) {
    for (const auto &testCase : cases) {
      auto matrix = std::string{};
      if (testCase.mapped) {
        // Route all three components identically: their sum must reproduce
        // the input with the requested polarity and gain, independent of the split.
        for (auto i = 0u; i < 256u; ++i) {
          if (i != 0) matrix += ',';
          matrix += i == (testCase.channels - 1u) * 16u ? "-0.5" : "0";
        }
      }
      const auto matrices = testCase.mapped ?
          ",\"ep\":false,\"dm\":[" + matrix + "],\"fm\":[" + matrix +
          "],\"rm\":[" + matrix + "]" : "";
      const auto routing = testCase.transfer ? ",\"inputBus\":0,\"outputBus\":1" : "";
      const auto returnNode = testCase.transfer ?
          ",{\"name\":\"Volume\",\"channel\":\"All\",\"inputBus\":1,\"outputBus\":0}" : "";
      const auto path = writePreset(directory, "spatial.effetune_preset",
          "{\"pipeline\":[{\"name\":\"Spatial Mapper\",\"channel\":\"" +
          std::string(testCase.selection) + "\",\"parameters\":{\"ic\":" + std::string(testCase.mapped ? "2" : "16") + ",\"bd\":\"" +
          std::string(testCase.bands) + "\"" + matrices + "}" + routing + "}" + returnNode + "]}");
      auto loaded = pipetune::loadDspPipeline(path,
          {.sampleRate = testCase.rate, .maxChannels = testCase.channels, .maxFrames = 257}, backend);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->latencyFrames() == testCase.latency,
                 "Spatial Mapper must report sample-rate-dependent latency")) return false;
      const auto frames = testCase.latency + 4099u;
      for (auto start = 0u; start < frames;) {
        const auto count = std::min(257u, frames - start);
        auto samples = std::vector<float>(testCase.channels * count);
        for (auto ch = 0u; ch < testCase.channels; ++ch)
          for (auto i = 0u; i < count; ++i)
            samples[ch * count + i] = start + i == 173u ? (ch + 1u) * 0.01F : 0.0F;
        if (!check(loaded.pipeline->process(samples, testCase.channels, count,
                    static_cast<double>(start) / testCase.rate) == pipetune::ProcessStatus::ok,
                   "Spatial Mapper must process complete and partial blocks")) return false;
        for (auto ch = 0u; ch < testCase.channels; ++ch) {
          auto gain = (ch + 1u) * 0.01F;
          if (testCase.mapped) {
            if (ch < 2u) gain = 0.0F;
            if (ch == testCase.channels - 1u) gain = -0.005F;
          }
          if (testCase.transfer) gain *= 2.0F;
          for (auto i = 0u; i < count; ++i) {
            const auto expected = start + i == testCase.latency + 173u ? gain : 0.0F;
            if (!check(approximately(samples[ch * count + i], expected, 3.0e-5F),
                       "Spatial Mapper PCM routing, polarity, silence or delay differs")) return false;
          }
        }
        start += count;
      }
      const auto nextRate = testCase.rate == 48000.0F ? 96000.0F : 48000.0F;
      auto rebuilt = pipetune::rebuildDspPipeline(*loaded.pipeline,
          {.sampleRate = nextRate, .maxChannels = testCase.channels, .maxFrames = 257}, backend);
      if (!check(rebuilt.pipeline != nullptr, rebuilt.error) ||
          !check(rebuilt.pipeline->latencyFrames() == (nextRate == 48000.0F ? 2560u : 5120u),
                 "Spatial Mapper rebuild must refresh its latency")) return false;
    }
  }
  return true;
}

static bool testTvAudioSimulator(const std::filesystem::path &directory) {
  const auto rates = std::array{44100.0F, 48000.0F, 88200.0F, 96000.0F,
                                176400.0F, 192000.0F, 352800.0F, 384000.0F};
  const auto standards = std::array{"M/EIA-J", "M/BTSC", "M/A2", "B/G A2",
                                   "B/G NICAM", "I NICAM", "D/K Mono", "L AM"};
  const auto backends = pipetune::discoverDspBackends();
  for (auto i = std::size_t{0}; i < rates.size(); ++i) {
    const auto path = writePreset(directory, "tv.effetune_preset",
        "{\"pipeline\":[{\"name\":\"TV Audio Simulator\",\"parameters\":{\"ss\":\"" +
        std::string(standards[i]) + "\",\"tx\":\"" + (i % 2 == 0 ? "Dual" : "Mono") +
        "\",\"sm\":\"" + (i % 2 == 0 ? "Sub" : "Main") + "\"},\"channel\":\"All\"}]}");
    auto loaded = pipetune::loadDspPipeline(path,
        {.sampleRate = rates[i], .maxChannels = 2, .maxFrames = 127});
    if (!check(loaded.pipeline != nullptr, loaded.error) ||
        !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1,
               "TV Audio Simulator must load every supported rate and standard")) return false;
    const auto latency = loaded.pipeline->latencyFrames();
    if (!check(latency > 0, "TV Audio Simulator must expose its conversion latency")) return false;
    auto energy = 0.0;
    for (auto block = 0u; block < 64u; ++block) {
      auto audio = std::vector<float>(254);
      for (auto j = 0u; j < audio.size(); ++j)
        audio[j] = 0.1F * std::sin(static_cast<float>(block * 127u + j) * 0.05F);
      if (!check(loaded.pipeline->process(audio, 2, 127,
                  static_cast<double>(block * 127u) / rates[i]) == pipetune::ProcessStatus::ok,
                 "TV Audio Simulator must process its supported rates")) return false;
      for (const auto value : audio) energy += value * value;
    }
    if (!check(std::isfinite(energy) && energy > 0.001,
               "TV Audio Simulator standards must produce finite nonzero audio")) return false;
    const auto backend = backends.simd.backend != nullptr ? backends.simd.backend : backends.scalar.backend;
    const auto rebuilt = pipetune::rebuildDspPipeline(*loaded.pipeline,
        {.sampleRate = rates[i], .maxChannels = 2, .maxFrames = 127}, backend);
    if (!check(rebuilt.pipeline != nullptr, rebuilt.error) ||
        !check(rebuilt.pipeline->latencyFrames() == latency,
               "TV Audio Simulator backend rebuild must preserve latency")) return false;
    const auto rejected = pipetune::rebuildDspPipeline(*loaded.pipeline,
        {.sampleRate = 32000.0F, .maxChannels = 2, .maxFrames = 127});
    if (!check(rejected.pipeline == nullptr && !rejected.error.empty() && rejected.warnings.empty(),
               "unsupported TV rates must fail instead of silently omitting the effect") ||
        !check(loaded.pipeline->sampleRate() == rates[i],
               "failed TV rebuild must preserve the original pipeline")) return false;
  }
  // Mix zero keeps the dry path's delay. A pair selection also compensates
  // untouched channels; All delegates the extra-channel behavior to the kernel.
  for (const auto selection : {"All", "34", "3"}) {
    const auto path = writePreset(directory, "tv-dry.effetune_preset",
        "{\"pipeline\":[{\"name\":\"TV Audio Simulator\",\"parameters\":{\"mx\":0},\"channel\":\"" +
        std::string(selection) + "\"}]}");
    auto loaded = pipetune::loadDspPipeline(path,
        {.sampleRate = 48000.0F, .maxChannels = 6, .maxFrames = 128});
    if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
    const auto latency = loaded.pipeline->latencyFrames();
    for (auto start = 0u; start < latency + 256u; start += 128u) {
      auto audio = std::vector<float>(6u * 128u, 0.0F);
      if (start == 0) for (auto ch = 0u; ch < 6; ++ch) audio[ch * 128u + 11] = 0.1F;
      if (!check(loaded.pipeline->process(audio, 6, 128, static_cast<double>(start) / 48000.0) ==
                     pipetune::ProcessStatus::ok, "TV dry-path routing failed")) return false;
      for (auto ch = 0u; ch < 6; ++ch) {
        const auto delay = std::string_view(selection) == "All" && ch >= 2u ? 0u : latency;
        for (auto frame = 0u; frame < 128; ++frame)
          if (!check(approximately(audio[ch * 128u + frame], start + frame == delay + 11u ? 0.1F : 0.0F),
                     "TV dry-path and extra-channel latency must match the selected route")) return false;
      }
    }
  }
  return true;
}

static bool testEffeTune210Pipeline(const std::filesystem::path &directory) {
  for (const auto name : {"Spatial Mapper", "TV Audio Simulator"}) {
    const auto path = writePreset(directory, "effetune-2.10.effetune_preset",
        "{\"pipeline\":[{\"name\":\"" + std::string(name) + "\"}]}");
    auto loaded = pipetune::loadDspPipeline(
        path, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 128});
    if (!check(loaded.pipeline != nullptr, loaded.error) ||
        !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1,
               "EffeTune 2.10 audio processors must execute without warnings") ||
        !check(loaded.pipeline->latencyFrames() > 0,
               "EffeTune 2.10 audio processors must report their latency")) return false;
    auto energy = 0.0;
    for (auto block = 0u; block < 64u; ++block) {
      auto samples = std::vector<float>(256);
      for (auto i = 0u; i < samples.size(); ++i)
        samples[i] = 0.2F * std::sin(static_cast<float>(block * 128u + i) * 0.1F);
      if (!check(loaded.pipeline->process(samples, 2, 128,
                  static_cast<double>(block * 128u) / 48000.0) == pipetune::ProcessStatus::ok,
                 "EffeTune 2.10 preset processing failed")) return false;
      for (const auto sample : samples) energy += sample * sample;
    }
    if (!check(std::isfinite(energy) && energy > 0.01,
               "EffeTune 2.10 processors must render finite audible output")) return false;
  }
  return true;
}

static bool testEffeTune211Processor(const std::filesystem::path &directory,
                                    std::string_view name) {
  const auto attack = name == "Attack Tonal Balance";
  const auto path = writePreset(directory, "effetune-2.11.effetune_preset",
      "{\"pipeline\":[{\"name\":\"" + std::string(name) + "\",\"channel\":\"All\"}]}");
  auto loaded = pipetune::loadDspPipeline(path,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 127});
  if (!check(loaded.pipeline != nullptr, loaded.error) ||
      !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1,
             std::string(name) + " must execute without warnings") ||
      !check(loaded.pipeline->latencyFrames() == (attack ? 5120u : 0u),
             std::string(name) + " must report the documented latency")) return false;

  auto energy = 0.0;
  auto difference = 0.0;
  // Render past the analysis delay with an irregular final block. Compare the
  // neutral Attack output with delayed input, not just its silent prefix.
  for (auto start = 0u; start < 24000u;) {
    const auto count = std::min(127u, 24000u - start);
    auto audio = std::vector<float>(2u * count);
    for (auto ch = 0u; ch < 2u; ++ch)
      for (auto i = 0u; i < count; ++i)
        audio[ch * count + i] = 0.2F * std::sin(
            2.0 * std::numbers::pi * 100.0 * (start + i) / 48000.0);
    const auto input = audio;
    if (!check(loaded.pipeline->process(audio, 2, count, start / 48000.0) ==
                   pipetune::ProcessStatus::ok, "EffeTune 2.11 PCM processing failed")) return false;
    for (auto ch = 0u; ch < 2u; ++ch) {
      for (auto i = 0u; i < count; ++i) {
        const auto value = audio[ch * count + i];
        energy += value * value;
        difference += std::abs(value - input[ch * count + i]);
        if (attack) {
          const auto frame = static_cast<int>(start + i) - 5120;
          const auto expected = frame < 0 ? 0.0 : 0.2 * std::sin(
              2.0 * std::numbers::pi * 100.0 * frame / 48000.0);
          if (!check(approximately(value, expected, 2.0e-5F),
                     "neutral Attack Tonal Balance must reproduce the delayed input")) return false;
        }
      }
    }
    start += count;
  }
  return check(std::isfinite(energy) && energy > 1.0,
               "new processors must produce finite audible output after their latency") &&
         check(attack || difference > 1.0,
               "Bass Extender must add generated bass to the input");
}

static bool testBassExtenderProcessingWidth(const std::filesystem::path &directory) {
  for (const auto channels : {3u, 4u, 16u}) {
    const auto path = writePreset(directory, "bass-width.effetune_preset",
        R"json({"pipeline":[{"name":"Bass Extender","channel":"All"}]})json");
    const auto loaded = pipetune::loadDspPipeline(path,
        {.sampleRate = 48000.0F, .maxChannels = channels, .maxFrames = 64});
    if (!check(loaded.pipeline == nullptr && loaded.warnings.empty() &&
                   loaded.error.find("Bass Extender") != std::string::npos &&
                   loaded.error.find("one or two") != std::string::npos,
               "Bass Extender must reject processing widths above two with a named error")) return false;
  }
  return true;
}

static bool testRetainedRecipeRebuild(
    const std::filesystem::path &directory) {
  const auto path = writePreset(
      directory, "retained.effetune_preset",
      R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}]})json");
  auto loaded = pipetune::loadDspPipeline(
      path,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(loaded.pipeline != nullptr, loaded.error)) {
    return false;
  }
  std::filesystem::remove(path);
  auto rebuilt = pipetune::rebuildDspPipeline(
      *loaded.pipeline,
      {.sampleRate = 96000.0F, .maxChannels = 2, .maxFrames = 64});
  return check(rebuilt.pipeline != nullptr, rebuilt.error) &&
         check(rebuilt.pipeline->sampleRate() == 96000.0F,
               "rebuild must use the requested rate") &&
         check(rebuilt.pipeline->activePluginCount() == 1,
               "rebuild must retain the preset recipe after file removal");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-preset-test-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);

  const auto attack = testEffeTune211Processor(directory, "Attack Tonal Balance");
  const auto bass = testEffeTune211Processor(directory, "Bass Extender");
  const auto bassWidth = testBassExtenderProcessingWidth(directory);
  const auto visualizers = testVisualizationPresets(directory);
  const auto passed = attack && bass && bassWidth && visualizers &&
      testFirCrossoverRouting(directory) && testFirCrossoverBands(directory) &&
      testEffeTune210Pipeline(directory) && testSpatialMapper(directory) && testTvAudioSimulator(directory) &&
      testBypassPipeline() && testCanonicalPreset(directory) &&
      testLegacyPreset(directory) && testEffeTune26Pipeline(directory) &&
      testGeneratedAssetDsp(directory) &&
      testTubeSimulator25Models(directory) &&
      testTubeSimulatorLatency(directory) &&
      testChannelLatencyCompensation(directory) &&
      testBalanceChannelPairs(directory) &&
      testRawLegacyPipeline(directory) && testRejectedInputs(directory) &&
      testRuntimeBounds(directory) && testEffeTune27Multichannel(directory) &&
      testRetainedRecipeRebuild(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
