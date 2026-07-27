#include "pipetune/dsp_pipeline.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
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
      pipetune::loadDspPipeline(path, {.sampleRate = 192000.0F, .maxChannels = 8, .maxFrames = 32});
  if (!check(result.pipeline != nullptr, result.error)) {
    return false;
  }
  auto samples = std::vector<float>(32, 0.25F);
  if (!check(result.pipeline->sampleRate() == 192000.0F,
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

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-preset-test-" + std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);

  const auto passed = testCanonicalPreset(directory) && testLegacyPreset(directory) &&
                      testRawLegacyPipeline(directory) && testRejectedInputs(directory) &&
                      testRuntimeBounds(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
