/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/dsp_pipeline.h"

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
      !check(result.pipeline->activePluginCount() == 23,
             "EffeTune generated-asset DSP nodes must become active") ||
      !check(result.warnings.size() == 3,
             "only unresolved and incompatible asset DSP nodes must be omitted") ||
      !check(containsWarning(result.warnings, "FIR Crossover") &&
                 containsWarning(result.warnings, "Room EQ") &&
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
          R"json({"lt":"0","bc":2,"pm":"min","tp":8192,"f1":1200,"s1":-48})json",
          4u, 0u, true, false},
      GeneratedAssetCase{
          "five-band-fir-peq.effetune_preset", "5Band FIR PEQ",
          R"json({"lt":"0","pm":"min","tp":8192,"f2":1000,"g2":6,"q2":1,"t2":"pk","e2":true})json",
          2u, 0u, false, true},
      GeneratedAssetCase{
          "group-delay-eq.effetune_preset", "Group Delay EQ",
          R"json({"lt":"0","tp":4096,"d7":2})json", 2u, 2048u, false,
          false},
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
      pipetune::loadDspPipeline(valid, {.sampleRate = 48000.0F, .maxChannels = 9, .maxFrames = 64});
  return check(invalidRate.pipeline == nullptr, "sample rates below 32 kHz must fail") &&
         check(invalidChannels.pipeline == nullptr, "more than eight channels must fail");
}

static bool testRuntimeBounds(const std::filesystem::path &directory) {
  const auto path =
      writePreset(directory, "bounds.effetune_preset", R"json({"pipeline":[]})json");
  const auto result =
      pipetune::loadDspPipeline(path, {.sampleRate = 384000.0F, .maxChannels = 8, .maxFrames = 32});
  if (!check(result.pipeline != nullptr, result.error)) {
    return false;
  }
  auto samples = std::vector<float>(32, 0.25F);
  if (!check(result.pipeline->sampleRate() == 384000.0F,
             "pipeline must report its prepared sample rate") ||
      !check(result.pipeline->process(samples, 8, 4, 0.0) == pipetune::ProcessStatus::ok,
             "maximum supported channel/rate configuration must process") ||
      !check(std::ranges::all_of(samples, [](float value) { return value == 0.25F; }),
             "an empty pipeline must be transparent")) {
    return false;
  }
  auto oversized = std::vector<float>(8u * 33u, 0.25F);
  return check(result.pipeline->process(oversized, 8, 33, 0.0) ==
                   pipetune::ProcessStatus::invalidBuffer,
               "processing beyond the prepared frame count must fail safely");
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

  const auto passed =
      testBypassPipeline() && testCanonicalPreset(directory) &&
      testLegacyPreset(directory) && testEffeTune26Pipeline(directory) &&
      testGeneratedAssetDsp(directory) &&
      testTubeSimulator25Models(directory) &&
      testTubeSimulatorLatency(directory) &&
      testChannelLatencyCompensation(directory) &&
      testBalanceChannelPairs(directory) &&
      testRawLegacyPipeline(directory) && testRejectedInputs(directory) &&
      testRuntimeBounds(directory) && testRetainedRecipeRebuild(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
