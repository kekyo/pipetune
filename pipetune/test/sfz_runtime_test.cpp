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
#include <bit>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numbers>
#include <stdexcept>
#include <unistd.h>

static void require(bool condition, std::string_view message) {
  if (!condition) throw std::runtime_error(std::string(message));
}

static std::filesystem::path writePreset(const std::filesystem::path &directory,
    std::string_view channel, std::string_view parameters) {
  const auto path = directory / "test.effetune_preset";
  std::ofstream(path) << "{\"pipeline\":[{\"name\":\"SFZ Note Player\",\"channel\":\"" << channel <<
      "\",\"parameters\":{\"sf\":\"0123456789abcdef01234567\",\"th\":0.01,\"mn\":83,\"mx\":83," << parameters << "}}]}";
  return path;
}

static std::vector<float> render(pipetune::DspPipeline &pipeline, std::span<const float> input,
    std::uint32_t channels, std::uint32_t blockFrames) {
  const auto frames = static_cast<std::uint32_t>(input.size() / channels);
  auto output = std::vector<float>(input.size()), block = std::vector<float>(channels * blockFrames);
  for (auto offset = 0u; offset < frames; offset += blockFrames) {
    const auto count = std::min(blockFrames, frames - offset);
    for (auto channel = 0u; channel < channels; ++channel)
      std::copy_n(input.begin() + channel * frames + offset, count, block.begin() + channel * count);
    require(pipeline.process(std::span(block).first(channels * count), channels, count,
        offset / static_cast<double>(pipeline.sampleRate())) == pipetune::ProcessStatus::ok, "SFZ partial block failed");
    for (auto channel = 0u; channel < channels; ++channel)
      std::copy_n(block.begin() + channel * count, count, output.begin() + channel * frames + offset);
  }
  require(std::ranges::all_of(output, [](float value) { return std::isfinite(value); }), "SFZ output must remain finite");
  return output;
}

static double amplitude(std::span<const float> samples, std::uint32_t rate, double frequency) {
  auto sine = 0.0, cosine = 0.0;
  for (auto frame = std::size_t{0}; frame < samples.size(); ++frame) {
    const auto phase = 2 * std::numbers::pi * frequency * frame / rate;
    sine += samples[frame] * std::sin(phase);
    cosine += samples[frame] * std::cos(phase);
  }
  return 2 * std::hypot(sine, cosine) / samples.size();
}

static void testPlayback(const std::filesystem::path &directory, const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend) {
  struct Scenario { std::uint32_t rate, channels, first, width; const char *selection; };
  for (const auto &scenario : std::array<Scenario, 8>{{
      {44100, 2, 0, 2, "All"}, {48000, 1, 0, 1, "All"}, {96000, 2, 0, 2, "All"},
      {192000, 2, 0, 2, "All"}, {384000, 2, 0, 2, "All"}, {48000, 16, 14, 2, "1516"},
      {48000, 16, 15, 1, "16"}, {48000, 16, 0, 16, "All"}}}) {
    const auto path = writePreset(directory, scenario.selection, "\"dm\":0,\"wm\":100");
    auto loaded = pipetune::loadDspPipeline(path, {static_cast<float>(scenario.rate), scenario.channels, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    require(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1, "SFZ must be active without exclusions");
    const auto frames = scenario.rate / 2 + 13;
    auto input = std::vector<float>(scenario.channels * frames);
    for (auto channel = 0u; channel < scenario.channels; ++channel) {
      if (channel < scenario.first || channel >= scenario.first + scenario.width) {
        input[channel * frames + 17] = (channel + 1) / 64.0F;
      } else for (auto frame = 0u; frame < frames; ++frame)
        input[channel * frames + frame] = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * 1000 * frame / scenario.rate));
    }
    const auto output = render(*loaded.pipeline, input, scenario.channels, 257);
    const auto latency = loaded.pipeline->latencyFrames();
    require(latency > 0, "SFZ analysis latency must reach the pipeline");
    for (auto channel = 0u; channel < scenario.channels; ++channel) {
      const auto samples = std::span(output).subspan(channel * frames, frames);
      if (channel < scenario.first || channel >= scenario.first + scenario.width) {
        for (auto frame = 0u; frame < frames; ++frame)
          require(samples[frame] == (frame == latency + 17 ? (channel + 1) / 64.0F : 0.0F),
              "unselected SFZ channels must preserve their PCM with final latency alignment");
      } else if (channel < scenario.first + 2) {
        const auto tail = samples.subspan(scenario.rate / 3);
        const auto frequency = channel == scenario.first ? 700 : 900;
        require(amplitude(tail, scenario.rate, frequency) > 0.035, "registered stereo instrument did not sound at its original pitch");
        require(amplitude(tail, scenario.rate, 1000) < 0.01, "wet-only SFZ leaked the detected input signal");
      } else require(std::ranges::all_of(samples, [](float value) { return value == 0; }),
          "All-channel SFZ wet output must follow the upstream first-pair contract");
    }
    require(loaded.pipeline->reset() == pipetune::ProcessStatus::ok, "SFZ reset failed");
    const auto replay = render(*loaded.pipeline, input, scenario.channels, 257);
    require(replay == output, "SFZ reset must clear note state while retaining the prepared instrument");
    auto rebuilt = pipetune::rebuildDspPipeline(*loaded.pipeline, {96000, scenario.channels, 257});
    require(rebuilt.pipeline != nullptr, rebuilt.error);
    require(rebuilt.cachedAssetCount == 1 && rebuilt.pipeline->activePluginCount() == 1,
        "rate rebuilding must reuse the original-rate SFZ bank");
    std::cout << "SFZ rate=" << scenario.rate << " channels=" << scenario.channels <<
        " selection=" << scenario.selection << " latency=" << latency << '\n';
  }
}

static void testDryAndMix(const std::filesystem::path &directory, const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend) {
  auto baseline = std::uint32_t{0};
  for (const auto timing : {0, -100, 100}) {
    const auto path = writePreset(directory, "1516", "\"dm\":100,\"wm\":0,\"tm\":" + std::to_string(timing));
    auto loaded = pipetune::loadDspPipeline(path, {48000, 16, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    const auto latency = loaded.pipeline->latencyFrames();
    if (timing == 0) baseline = latency;
    require(latency == baseline + (timing < 0 ? 4800u : 0u), "negative SFZ timing must increase the reported dry latency");
    const auto frames = latency + 1000;
    auto input = std::vector<float>(16 * frames);
    for (auto channel = 0u; channel < 16; ++channel) input[channel * frames + 17] = (channel + 1) / 32.0F;
    const auto output = render(*loaded.pipeline, input, 16, 97);
    for (auto channel = 0u; channel < 16; ++channel)
      for (auto frame = 0u; frame < frames; ++frame)
        require(output[channel * frames + frame] == (frame == latency + 17 ? (channel + 1) / 32.0F : 0.0F),
            "SFZ dry timing and final alignment must preserve each channel impulse");
  }
  auto input = std::vector<float>(2 * 24013);
  for (auto channel = 0u; channel < 2; ++channel)
    for (auto frame = 0u; frame < 24013; ++frame)
      input[channel * 24013 + frame] = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * 1000 * frame / 48000.0));
  const auto wetPath = writePreset(directory, "All", "\"dm\":0,\"wm\":100");
  auto wet = pipetune::loadDspPipeline(wetPath, {48000, 2, 257}, backend, context);
  require(wet.pipeline != nullptr, wet.error);
  const auto wetAudio = render(*wet.pipeline, input, 2, 97);
  const auto mixPath = writePreset(directory, "All", "\"dm\":50,\"wm\":75,\"og\":-6");
  auto mix = pipetune::loadDspPipeline(mixPath, {48000, 2, 257}, backend, context);
  require(mix.pipeline != nullptr, mix.error);
  const auto mixedAudio = render(*mix.pipeline, input, 2, 97);
  for (auto channel = 0u; channel < 2; ++channel)
    for (auto frame = 0u; frame < 24013; ++frame) {
      const auto at = channel * 24013 + frame;
      const auto dry = frame >= mix.pipeline->latencyFrames() ? input[at - mix.pipeline->latencyFrames()] : 0;
      const auto expected = (dry * 0.5 + wetAudio[at] * 0.75) * std::pow(10.0, -6.0 / 20);
      require(std::abs(mixedAudio[at] - expected) < 2e-6, "independent SFZ dry, wet, and output gains differ");
    }
}

static void testIdle(const std::filesystem::path &directory, const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend) {
  for (const auto timeout : {0u, 100u}) {
    const auto path = writePreset(directory, "All", "\"dm\":0,\"wm\":100");
    auto loaded = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    auto slot = pipetune::DspPipelineSlot(std::move(loaded.pipeline));
    const auto policy = pipetune::DspIdlePolicy{timeout};
    auto block = std::vector<float>(2 * 257);
    auto total = 0u;
    const auto play = [&](bool silent, std::uint32_t frames) {
      auto audio = std::vector<float>{};
      for (auto offset = 0u; offset < frames; offset += 257) {
        const auto count = std::min(257u, frames - offset);
        for (auto frame = 0u; frame < count; ++frame) {
          const auto value = silent ? 0.0F : static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * 1000 * (offset + frame) / 48000.0));
          block[frame] = block[count + frame] = value;
        }
        const auto result = slot.processWithIdle(std::span(block).first(2 * count), 2, count, total / 48000.0, policy);
        require(result.status == pipetune::ProcessStatus::ok, "SFZ idle processing failed");
        audio.insert(audio.end(), block.begin(), block.begin() + count);
        total += count;
      }
      return audio;
    };
    const auto initial = play(false, 24013);
    require(amplitude(std::span(initial).subspan(16000), 48000, 700) > 0.05, "SFZ did not sound before suspension");
    const auto tail = play(true, 14400);
    const auto processed = slot.performanceCounters().processedFrames;
    if (timeout) {
      require(std::ranges::all_of(std::span(tail).subspan(6000), [](float value) { return value == 0; }),
          "SFZ suspension must fade and clear the active voice");
      static_cast<void>(play(true, 1000));
      require(slot.performanceCounters().processedFrames == processed, "sleeping SFZ must skip native processing");
    } else {
      require(processed == total && amplitude(std::span(tail).subspan(6000), 48000, 700) > 0.001,
          "ignored suspension must keep processing the SFZ release tail");
    }
    const auto resumed = play(false, 24013);
    require(amplitude(std::span(resumed).subspan(16000), 48000, 700) > 0.05,
        "SFZ must detect notes and reuse its bank after suspension");
  }
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() / ("pipetune-sfz-runtime-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  auto result = 0;
  try {
    constexpr auto frames = 4410u;
    auto wave = std::vector<std::uint8_t>(44 + frames * 8);
    const auto put = [&wave](std::size_t offset, std::uint32_t value, unsigned width) {
      for (auto index = 0u; index < width; ++index) wave[offset + index] = value >> (8 * index);
    };
    for (const auto &[offset, text] : std::array{std::pair{0u, "RIFF"}, std::pair{8u, "WAVE"},
        std::pair{12u, "fmt "}, std::pair{36u, "data"}}) std::copy_n(text, 4, wave.begin() + offset);
    put(4, wave.size() - 8, 4); put(16, 16, 4); put(20, 3, 2); put(22, 2, 2);
    put(24, 44100, 4); put(28, 352800, 4); put(32, 8, 2); put(34, 32, 2); put(40, frames * 8, 4);
    for (auto frame = 0u; frame < frames; ++frame)
      for (auto channel = 0u; channel < 2; ++channel)
        put(44 + (frame * 2 + channel) * 4, std::bit_cast<std::uint32_t>(static_cast<float>(
            0.2 * std::sin(2 * std::numbers::pi * (700 + channel * 200) * frame / 44100.0))), 4);
    std::ofstream(directory / "stereo.wav", std::ios::binary).write(reinterpret_cast<const char *>(wave.data()), wave.size());
    std::ofstream(directory / "main.sfz") << "<region> sample=stereo.wav key=83 amp_veltrack=0 loop_mode=loop_continuous ampeg_release=1";
    std::ofstream(directory / "sfz-references.json") << "[{\"id\":\"0123456789abcdef01234567\",\"name\":\"Stereo test\",\"root\":\"" <<
        directory.string() << "\",\"path\":\"" << (directory / "main.sfz").string() << "\"}]";
    const auto context = pipetune::PipelineLoadContext{.measurementDirectory = {}, .effetuneDirectory = directory,
        .assetCache = {.directory = directory / "cache"}};
    const auto discovered = pipetune::discoverDspBackends();
    auto backends = std::vector{discovered.scalar.backend};
    for (const auto &variant : discovered.simdVariants) if (variant.backend) backends.push_back(variant.backend);
    for (const auto &backend : backends) {
      require(backend != nullptr, "Scalar backend must be available");
      testPlayback(directory, context, backend);
      testDryAndMix(directory, context, backend);
      testIdle(directory, context, backend);
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  std::filesystem::remove_all(directory);
  return result;
}
