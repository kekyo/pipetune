/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_pipeline_slot.h"
#include "dsp_backend_loader.h"
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
#include <string>
#include <string_view>
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

static std::string withMode(std::string_view parameters, std::string_view mode) {
  return std::string(parameters) + ",\"dt\":\"" + std::string(mode) + "\"";
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

static float detectionInput(std::uint32_t frame, std::uint32_t rate, std::string_view mode) {
  if (mode == "Classic")
    return static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * 1000 * frame / rate));
  // Use note-centered harmonics for a confirmed Event at the registered key (83).
  const auto frequency = 440 * std::pow(2.0, (83.0 - 69.0) / 12.0);
  auto sample = 0.0;
  for (auto harmonic = 1u; harmonic <= 8u; ++harmonic)
    sample += 0.1 / harmonic * std::sin(
        2 * std::numbers::pi * frequency * harmonic * frame / rate +
        0.37 * harmonic * harmonic);
  return static_cast<float>(sample);
}

static std::uint32_t nativeSfzLatency(const pipetune::DspBackend &backend,
    std::uint32_t rate, std::uint32_t channels, int timing, std::string_view mode) {
  const auto *definition = pipetune::findDspByDisplayName("SFZ Note Player");
  require(definition != nullptr, "SFZ native definition is unavailable");
  const auto json = "{\"tm\":" + std::to_string(timing) +
      ",\"dt\":\"" + std::string(mode) + "\"}";
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  require(document != nullptr, "SFZ native latency parameters are invalid");
  const auto packed = pipetune::packDspParameters(*definition, yyjson_doc_get_root(document.get()));
  require(packed.error.empty(), packed.error);
  const auto &api = pipetune::dspBackendApi(backend);
  const auto engine = api.engineCreate();
  require(engine != 0, "SFZ native latency engine could not be created");
  auto instance = et_instance{0};
  try {
    require(api.enginePrepare(engine, rate, channels, 257, 0) == ET_OK,
        "SFZ native latency engine could not be prepared");
    instance = api.instanceCreate(engine, definition->typeName.data());
    require(instance != 0, "SFZ native latency instance could not be created");
    require(api.instanceSetParams(engine, instance, packed.floats.data(), packed.floats.size(),
        definition->hash, 0) == ET_OK, "SFZ native latency parameters were rejected");
    auto silence = std::vector<float>(channels);
    // The native instance applies staged parameters at the next process boundary.
    require(api.instanceProcess(engine, instance, silence.data(), channels, 1, 0) == ET_OK,
        "SFZ native latency parameters could not be applied");
    const auto latency = api.instanceLatency(engine, instance);
    api.instanceDestroy(engine, instance);
    api.engineDestroy(engine);
    return latency;
  } catch (...) {
    if (instance) api.instanceDestroy(engine, instance);
    api.engineDestroy(engine);
    throw;
  }
}

static void writeMonoWave(const std::filesystem::path &path, double frequency, double overtone) {
  constexpr auto frames = 48000u;
  auto wave = std::vector<std::uint8_t>(44 + frames * sizeof(float));
  const auto put = [&wave](std::size_t offset, std::uint32_t value, unsigned width) {
    for (auto index = 0u; index < width; ++index) wave[offset + index] = value >> (8 * index);
  };
  for (const auto &[offset, text] : std::array{std::pair{0u, "RIFF"}, std::pair{8u, "WAVE"},
      std::pair{12u, "fmt "}, std::pair{36u, "data"}}) std::copy_n(text, 4, wave.begin() + offset);
  put(4, wave.size() - 8, 4); put(16, 16, 4); put(20, 3, 2); put(22, 1, 2);
  put(24, 48000, 4); put(28, 48000 * 4, 4); put(32, 4, 2); put(34, 32, 2);
  put(40, frames * 4, 4);
  for (auto frame = 0u; frame < frames; ++frame) {
    const auto phase = 2 * std::numbers::pi * frequency * frame / 48000.0;
    const auto value = static_cast<float>(0.15 * std::sin(phase) + overtone * std::sin(2 * phase));
    put(44 + frame * 4, std::bit_cast<std::uint32_t>(value), 4);
  }
  std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char *>(wave.data()), wave.size());
}

static pipetune::PipelineLoadContext writeShiftInstrument(const std::filesystem::path &directory) {
  const auto root = directory / "shift-instrument";
  std::filesystem::create_directories(root);
  auto definition = std::ofstream(root / "main.sfz");
  for (const auto &[key, overtone] : std::array{std::pair{77, 0.02}, std::pair{83, 0.07},
      std::pair{89, 0.12}}) {
    const auto name = "key" + std::to_string(key) + ".wav";
    writeMonoWave(root / name, 700 * std::exp2((key - 83) / 12.0), overtone);
    definition << "<region> sample=" << name << " key=" << key <<
        " amp_veltrack=100 loop_mode=loop_continuous ampeg_release=0.2\n";
  }
  std::ofstream(root / "sfz-references.json") <<
      "[{\"id\":\"0123456789abcdef01234567\",\"name\":\"Shift test\",\"root\":\"" <<
      root.string() << "\",\"path\":\"" << (root / "main.sfz").string() << "\"}]";
  return {.measurementDirectory = {}, .effetuneDirectory = root,
      .assetCache = {.directory = root / "cache"}};
}

static void testSampleShiftAndPreview(const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend,
    const std::shared_ptr<const pipetune::DspBackend> &alternateBackend) {
  constexpr auto frames = 24013u;
  auto input = std::vector<float>(2 * frames);
  for (auto channel = 0u; channel < 2; ++channel)
    for (auto frame = 0u; frame < frames; ++frame)
      input[channel * frames + frame] = static_cast<float>(
          0.3 * std::sin(2 * std::numbers::pi * 1000 * frame / 48000.0));
  auto harmonics = std::array<double, 3>{};
  for (auto index = 0u; index < 3; ++index) {
    const auto shift = static_cast<int>(index) * 6 - 6;
    const auto path = writePreset(context.effetuneDirectory, "All",
        "\"dm\":0,\"wm\":100,\"ss\":" + std::to_string(shift));
    auto loaded = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    const auto output = render(*loaded.pipeline, input, 2, 257);
    const auto tail = std::span(output).subspan(16000, frames - 16000);
    require(amplitude(tail, 48000, 700) > 0.025,
        "sample shift must retain the detected note's output pitch");
    require(amplitude(tail, 48000, 700 * std::exp2(shift / 12.0)) < 0.01 || shift == 0,
        "sample shift must not transpose the selected source's original pitch");
    harmonics[index] = amplitude(tail, 48000, 1400) / amplitude(tail, 48000, 700);
  }
  require(harmonics[0] + 0.12 < harmonics[1] && harmonics[1] + 0.12 < harmonics[2],
      "sample shift must select the neighboring WAV timbre");

  const auto silence = std::vector<float>(2 * frames);
  const auto defaultPath = writePreset(context.effetuneDirectory, "All", "\"dm\":0,\"wm\":100");
  auto noPreview = pipetune::loadDspPipeline(defaultPath, {48000, 2, 257}, backend, context);
  require(noPreview.pipeline != nullptr, noPreview.error);
  const auto quiet = render(*noPreview.pipeline, silence, 2, 257);
  require(std::ranges::all_of(quiet, [](float sample) { return sample == 0; }),
      "default preview MIDI must leave a silent input silent");
  auto activePath = writePreset(context.effetuneDirectory, "All", "\"dm\":0,\"wm\":100,\"pm\":83,\"pv\":127");
  auto active = pipetune::loadDspPipeline(activePath, {48000, 2, 257}, backend, context);
  require(active.pipeline != nullptr, active.error);
  const auto loud = render(*active.pipeline, silence, 2, 257);
  const auto loudTail = std::span(loud).subspan(16000, frames - 16000);
  const auto loudFundamental = amplitude(loudTail, 48000, 700);
  require(loudFundamental > 0.05, "preview MIDI must sound without note detection input");
  auto softPath = writePreset(context.effetuneDirectory, "All", "\"dm\":0,\"wm\":100,\"pm\":83,\"pv\":32");
  auto soft = pipetune::loadDspPipeline(softPath, {48000, 2, 257}, backend, context);
  require(soft.pipeline != nullptr, soft.error);
  const auto softer = render(*soft.pipeline, silence, 2, 257);
  const auto softFundamental = amplitude(std::span(softer).subspan(16000, frames - 16000), 48000, 700);
  require(softFundamental > 0 && softFundamental < loudFundamental * 0.3,
      "preview velocity must change the actual sound level");
  require(active.pipeline->reset() == pipetune::ProcessStatus::ok, "SFZ preview reset failed");
  require(render(*active.pipeline, silence, 2, 257) == loud,
      "reset must stop the old preview voice and restart the retained preview note");
  auto rebuilt = pipetune::rebuildDspPipeline(*active.pipeline, {96000, 2, 257});
  require(rebuilt.pipeline != nullptr, rebuilt.error);
  require(rebuilt.cachedAssetCount == 1, "preview rebuild must reuse the prepared SFZ bank");
  const auto rebuiltSilence = std::vector<float>(2 * 48013);
  const auto rebuiltAudio = render(*rebuilt.pipeline, rebuiltSilence, 2, 257);
  require(amplitude(std::span(rebuiltAudio).subspan(32000, 16000), 96000, 700) > 0.05,
      "preview must restart at the same pitch after a sample-rate rebuild");
  auto stoppedPath = writePreset(context.effetuneDirectory, "All", "\"dm\":0,\"wm\":100,\"pm\":-1");
  auto stopped = pipetune::loadDspPipeline(stoppedPath, {48000, 2, 257}, backend, context);
  require(stopped.pipeline != nullptr, stopped.error);
  const auto stoppedAudio = render(*stopped.pipeline, silence, 2, 257);
  require(std::ranges::all_of(stoppedAudio, [](float sample) { return sample == 0; }),
      "a replacement preset with preview MIDI off must stop preview output");

  const auto routedPath = context.effetuneDirectory / "routed.effetune_preset";
  std::ofstream(routedPath) << R"({"pipeline":[{"name":"SFZ Note Player","channel":"All","inputBus":0,"outputBus":1,"parameters":{"sf":"0123456789abcdef01234567","th":0.01,"mn":83,"mx":83,"dm":0,"wm":100,"pm":83,"pv":127}},{"name":"Volume","channel":"All","inputBus":1,"outputBus":0,"parameters":{}}]})";
  auto routed = pipetune::loadDspPipeline(routedPath, {48000, 2, 257}, backend, context);
  require(routed.pipeline != nullptr, routed.error);
  require(routed.pipeline->latencyFrames() == active.pipeline->latencyFrames(),
      "SFZ on another bus must report the same final output latency");
  const auto routedAudio = render(*routed.pipeline, silence, 2, 257);
  for (auto frame = 0u; frame < routedAudio.size(); ++frame)
    require(std::abs(routedAudio[frame] - loud[frame]) < 2e-6,
        "SFZ preview on another bus must retain aligned stereo output");

  if (alternateBackend && alternateBackend != backend) {
    auto switched = pipetune::rebuildDspPipeline(*active.pipeline, {48000, 2, 257}, alternateBackend);
    require(switched.pipeline != nullptr, switched.error);
    require(switched.cachedAssetCount == 1 && switched.pipeline->backendVariant() == alternateBackend->variant(),
        "SFZ backend rebuild must retain the prepared bank and select the requested variant");
    const auto switchedAudio = render(*switched.pipeline, silence, 2, 257);
    require(amplitude(std::span(switchedAudio).subspan(16000, frames - 16000), 48000, 700) > 0.05,
        "SFZ preview must keep its pitch after a backend rebuild");
  }
}

static void testPlayback(const std::filesystem::path &directory, const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend, std::string_view mode) {
  struct Scenario { std::uint32_t rate, channels, first, width; const char *selection; };
  for (const auto &scenario : std::array<Scenario, 8>{{
      {44100, 2, 0, 2, "All"}, {48000, 1, 0, 1, "All"}, {96000, 2, 0, 2, "All"},
      {192000, 2, 0, 2, "All"}, {384000, 2, 0, 2, "All"}, {48000, 16, 14, 2, "1516"},
      {48000, 16, 15, 1, "16"}, {48000, 16, 0, 16, "All"}}}) {
    const auto path = writePreset(directory, scenario.selection,
        withMode("\"dm\":0,\"wm\":100", mode));
    auto loaded = pipetune::loadDspPipeline(path, {static_cast<float>(scenario.rate), scenario.channels, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    require(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1, "SFZ must be active without exclusions");
    const auto frames = scenario.rate / 2 + 13;
    auto input = std::vector<float>(scenario.channels * frames);
    for (auto channel = 0u; channel < scenario.channels; ++channel) {
      if (channel < scenario.first || channel >= scenario.first + scenario.width) {
        input[channel * frames + 17] = (channel + 1) / 64.0F;
      } else for (auto frame = 0u; frame < frames; ++frame)
        input[channel * frames + frame] = detectionInput(frame, scenario.rate, mode);
    }
    const auto output = render(*loaded.pipeline, input, scenario.channels, 257);
    const auto latency = loaded.pipeline->latencyFrames();
    require(latency > 0, "SFZ analysis latency must reach the pipeline");
    require(latency == nativeSfzLatency(*backend, scenario.rate, scenario.channels, 0, mode),
        "SFZ pipeline latency must match the native instance at each supported rate");
    if (mode == "Event") {
      const auto classic = nativeSfzLatency(*backend, scenario.rate, scenario.channels, 0, "Classic");
      const auto hop = static_cast<std::uint32_t>(std::lround(scenario.rate * 0.02));
      require(latency == classic + 7u * hop,
          "Event latency must add seven analysis hops at each supported rate");
    }
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
        require(amplitude(tail, scenario.rate, mode == "Event" ? 440 * std::exp2(14.0 / 12.0) : 1000) < 0.01, "wet-only SFZ leaked the detected input signal");
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
    if (scenario.channels <= 2) {
      const auto rebuiltFrames = 48013u;
      auto rebuiltInput = std::vector<float>(scenario.channels * rebuiltFrames);
      for (auto channel = 0u; channel < scenario.channels; ++channel)
        for (auto frame = 0u; frame < rebuiltFrames; ++frame)
          rebuiltInput[channel * rebuiltFrames + frame] = detectionInput(frame, 96000, mode);
      const auto rebuiltAudio = render(*rebuilt.pipeline, rebuiltInput, scenario.channels, 257);
      require(amplitude(std::span(rebuiltAudio).subspan(32000, 16000), 96000, 700) > 0.035,
          "rate rebuilding must retain audible SFZ output");
    }
    std::cout << "SFZ mode=" << mode << " rate=" << scenario.rate << " channels=" << scenario.channels <<
        " selection=" << scenario.selection << " latency=" << latency << '\n';
  }
}

static void testDryAndMix(const std::filesystem::path &directory, const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend, std::string_view mode) {
  auto baseline = std::uint32_t{0};
  for (const auto timing : {0, -100, 100}) {
    const auto path = writePreset(directory, "1516", withMode(
        "\"dm\":100,\"wm\":0,\"tm\":" + std::to_string(timing), mode));
    auto loaded = pipetune::loadDspPipeline(path, {48000, 16, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    const auto latency = loaded.pipeline->latencyFrames();
    if (timing == 0) baseline = latency;
    require(latency == nativeSfzLatency(*backend, 48000, 16, timing, mode),
        "SFZ timing latency must match the native instance report");
    require(latency == baseline + (timing < 0 ? 4800u : 0u),
        "negative SFZ timing must increase the reported dry latency");
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
      input[channel * 24013 + frame] = detectionInput(frame, 48000, mode);
  const auto wetPath = writePreset(directory, "All", withMode("\"dm\":0,\"wm\":100", mode));
  auto wet = pipetune::loadDspPipeline(wetPath, {48000, 2, 257}, backend, context);
  require(wet.pipeline != nullptr, wet.error);
  const auto wetAudio = render(*wet.pipeline, input, 2, 97);
  const auto mixPath = writePreset(directory, "All", withMode("\"dm\":50,\"wm\":75,\"og\":-6", mode));
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
    const std::shared_ptr<const pipetune::DspBackend> &backend, std::string_view mode) {
  for (const auto timeout : {0u, 100u}) {
    const auto path = writePreset(directory, "All", withMode("\"dm\":0,\"wm\":100", mode));
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
          const auto value = silent ? 0.0F : detectionInput(offset + frame, 48000, mode);
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

static void testRepeatedOnsetAndRelease(const std::filesystem::path &directory,
    const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend, std::string_view mode) {
  const auto path = writePreset(directory, "All", withMode("\"dm\":0,\"wm\":100", mode));
  auto single = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, context);
  auto repeated = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, context);
  require(single.pipeline != nullptr, single.error);
  require(repeated.pipeline != nullptr, repeated.error);
  constexpr auto frames = 96013u;
  auto firstInput = std::vector<float>(2 * frames), repeatedInput = std::vector<float>(2 * frames);
  for (auto channel = 0u; channel < 2; ++channel)
    for (auto frame = 0u; frame < frames; ++frame) {
      const auto tone = detectionInput(frame, 48000, mode);
      if (frame < 24000) firstInput[channel * frames + frame] = tone;
      if (frame < 24000 || (frame >= 48000 && frame < 72000))
        repeatedInput[channel * frames + frame] = tone;
    }
  const auto firstOutput = render(*single.pipeline, firstInput, 2, 257);
  const auto repeatedOutput = render(*repeated.pipeline, repeatedInput, 2, 257);
  const auto first = std::span(firstOutput).first(frames);
  const auto twice = std::span(repeatedOutput).first(frames);
  const auto offset = mode == "Event" ? 7u * 960u : 0u;
  const auto held = amplitude(first.subspan(16000 + offset, 6000), 48000, 700);
  const auto earlyRelease = amplitude(first.subspan(32000 + offset, 4000), 48000, 700);
  const auto lateRelease = amplitude(first.subspan(42000 + offset, 4000), 48000, 700);
  require(held > 0.035 && earlyRelease > lateRelease * 2 && lateRelease > 0,
      "a held SFZ note must sound continuously, then decay after note-off");
  auto renewed = std::vector<float>(6000);
  for (auto frame = 0u; frame < renewed.size(); ++frame)
    renewed[frame] = twice[64000 + offset + frame] - first[64000 + offset + frame];
  require(amplitude(renewed, 48000, 700) > 0.025,
      "a second onset must create a new audible SFZ voice after the first release");
  require(amplitude(twice.subspan(80000 + offset, 4000), 48000, 700) <
      amplitude(twice.subspan(68000 + offset, 4000), 48000, 700),
      "the repeated SFZ note must enter release after its second note-off");
}

static void testModeChangeAndRouting(const std::filesystem::path &directory,
    const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend,
    const std::shared_ptr<const pipetune::DspBackend> &alternateBackend) {
  auto isolated = context;
  isolated.assetCache.directory = directory / "mode-change-cache";
  std::filesystem::remove_all(isolated.assetCache.directory);
  const auto silence = std::vector<float>(2 * 24013);
  auto classicLatency = std::uint32_t{0};
  for (const auto mode : {"Classic", "Event", "Classic"}) {
    const auto path = writePreset(directory, "All",
        withMode("\"dm\":0,\"wm\":100,\"pm\":83,\"pv\":127", mode));
    auto loaded = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, isolated);
    require(loaded.pipeline != nullptr, loaded.error);
    require(loaded.cachedAssetCount == (classicLatency == 0 ? 0u : 1u),
        "changing only the detection mode must reuse the SFZ bank");
    const auto latency = loaded.pipeline->latencyFrames();
    require(latency == nativeSfzLatency(*backend, 48000, 2, 0, mode),
        "mode switching must preserve native latency reporting");
    if (classicLatency == 0) classicLatency = latency;
    else if (std::string_view(mode) == "Event")
      require(latency == classicLatency + 7u * 960u,
          "Event latency must add seven analysis hops at 48 kHz");
    else require(latency == classicLatency,
        "returning to Classic must restore its latency");
    const auto direct = render(*loaded.pipeline, silence, 2, 257);
    require(amplitude(std::span(direct).subspan(16000, 6000), 48000, 700) > 0.05,
        "preset mode switching must retain audible SFZ output");

    const auto routedPath = directory / "mode-routed.effetune_preset";
    std::ofstream(routedPath) <<
        "{\"pipeline\":[{\"name\":\"SFZ Note Player\",\"channel\":\"All\",\"inputBus\":0,\"outputBus\":1,"
        "\"parameters\":{\"sf\":\"0123456789abcdef01234567\",\"th\":0.01,\"mn\":83,\"mx\":83,"
        "\"dm\":0,\"wm\":100,\"pm\":83,\"pv\":127,\"dt\":\"" << mode <<
        "\"}},{\"name\":\"Volume\",\"channel\":\"All\",\"inputBus\":1,\"outputBus\":0,\"parameters\":{}}]}";
    auto routed = pipetune::loadDspPipeline(routedPath, {48000, 2, 257}, backend, isolated);
    require(routed.pipeline != nullptr, routed.error);
    require(routed.pipeline->latencyFrames() == latency,
        "another bus must preserve the selected SFZ mode's output latency");
    const auto routedAudio = render(*routed.pipeline, silence, 2, 257);
    for (auto frame = 0u; frame < direct.size(); ++frame)
      require(std::abs(routedAudio[frame] - direct[frame]) < 2e-6,
          "another bus must preserve aligned SFZ stereo output");

    if (alternateBackend && alternateBackend != backend) {
      auto switched = pipetune::rebuildDspPipeline(*loaded.pipeline,
          {48000, 2, 257}, alternateBackend);
      require(switched.pipeline != nullptr, switched.error);
      require(switched.cachedAssetCount == 1 &&
          switched.pipeline->backendVariant() == alternateBackend->variant(),
          "backend rebuilding must reuse the prepared SFZ bank");
      const auto switchedAudio = render(*switched.pipeline, silence, 2, 257);
      require(amplitude(std::span(switchedAudio).subspan(16000, 6000), 48000, 700) > 0.05,
          "backend rebuilding must preserve the selected SFZ mode's audio");
    }
  }
}

static void testEventPreviewStart(const std::filesystem::path &directory,
    const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend) {
  const auto silence = std::vector<float>(2 * 2048);
  auto reference = std::vector<float>{};
  for (const auto mode : {"Classic", "Event"}) {
    const auto path = writePreset(directory, "All",
        withMode("\"dm\":0,\"wm\":100,\"pm\":83,\"pv\":127", mode));
    auto loaded = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    const auto output = render(*loaded.pipeline, silence, 2, 257);
    if (reference.empty()) reference = output;
    else {
      require(std::ranges::any_of(output, [](float value) { return value != 0; }),
          "Event preview must play immediately without a detected note");
      require(output == reference,
          "Event analysis initialization must not advance preview sample playback");
      require(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
          "Event preview reset failed");
      require(render(*loaded.pipeline, silence, 2, 257) == reference,
          "Event preview must restart at the original attack after reset");
    }
  }
}

static void testTwoEventInstances(const std::filesystem::path &directory,
    const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend) {
  const auto path = directory / "two-event-instances.effetune_preset";
  std::ofstream(path) << R"json({"pipeline":[
    {"name":"SFZ Note Player","parameters":{
      "sf":"0123456789abcdef01234567","th":0.01,"mn":83,"mx":83,
      "dm":0,"wm":100,"dt":"Event","pm":83,"pv":127}},
    {"name":"SFZ Note Player","channel":"34","parameters":{
      "sf":"0123456789abcdef01234567","th":0.01,"mn":83,"mx":83,
      "dm":0,"wm":100,"dt":"Event"}}]})json";
  auto loaded = pipetune::loadDspPipeline(path, {48000, 4, 257}, backend, context);
  require(loaded.pipeline != nullptr, loaded.error);
  require(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 2,
      "both Event SFZ instances must prepare");
  constexpr auto frames = 60013u;
  auto input = std::vector<float>(4 * frames);
  for (auto frame = 0u; frame < frames; ++frame) {
    const auto sample = detectionInput(frame, 48000, "Event");
    input[2 * frames + frame] = sample;
    input[3 * frames + frame] = sample;
  }
  const auto output = render(*loaded.pipeline, input, 4, 257);
  for (auto channel = 0u; channel < 4; ++channel) {
    const auto tail = std::span(output).subspan(channel * frames + 30000, 20000);
    require(amplitude(tail, 48000, channel % 2 == 0 ? 700 : 900) > 0.035,
        "both Event SFZ pairs must sound after initial processing");
  }
  require(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
      "two Event SFZ instances could not reset");
  require(render(*loaded.pipeline, input, 4, 257) == output,
      "both Event SFZ instances must reproduce their PCM after reset");
}

static void testEventNoteHold(const std::filesystem::path &directory,
    const pipetune::PipelineLoadContext &context,
    const std::shared_ptr<const pipetune::DspBackend> &backend) {
  constexpr auto frames = 96013u;
  auto input = std::vector<float>(2 * frames);
  for (auto channel = 0u; channel < 2; ++channel)
    for (auto frame = 0u; frame < frames; ++frame)
      if (frame < 24000u || (frame >= 28800u && frame < 72000u))
        input[channel * frames + frame] = detectionInput(frame, 48000, "Event");
  auto outputs = std::array<std::vector<float>, 2>{};
  for (auto index = 0u; index < outputs.size(); ++index) {
    const auto hold = index == 0 ? 0 : 100;
    const auto path = writePreset(directory, "All", withMode(
        "\"dm\":0,\"wm\":100,\"nh\":" + std::to_string(hold), "Event"));
    auto loaded = pipetune::loadDspPipeline(path, {48000, 2, 257}, backend, context);
    require(loaded.pipeline != nullptr, loaded.error);
    outputs[index] = render(*loaded.pipeline, input, 2, 257);
    const auto first = std::span(outputs[index]).first(frames);
    require(amplitude(first.subspan(16000, 6000), 48000, 700) > 0.035 &&
            amplitude(first.subspan(62000, 6000), 48000, 700) > 0.035,
        "Event must sound before and after the short same-key gap");
    require(loaded.pipeline->reset() == pipetune::ProcessStatus::ok,
        "Event Note Hold reset failed");
    require(render(*loaded.pipeline, input, 2, 257) == outputs[index],
        "Event reset must clear Note Hold and retrigger state");
  }
  const auto firstWithoutHold = std::span(outputs[0]).first(frames);
  const auto firstWithHold = std::span(outputs[1]).first(frames);
  auto difference = 0.0;
  for (auto frame = 32000u; frame < 56000u; ++frame)
    difference += std::abs(firstWithoutHold[frame] - firstWithHold[frame]);
  require(difference > 0.01,
      "Event Note Hold must affect the audible release and same-key reattack");
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
    const auto shiftContext = writeShiftInstrument(directory);
    const auto discovered = pipetune::discoverDspBackends();
    auto backends = std::vector{discovered.scalar.backend};
    for (const auto &variant : discovered.simdVariants) if (variant.backend) backends.push_back(variant.backend);
    for (const auto &backend : backends) {
      require(backend != nullptr, "Scalar backend must be available");
      for (const auto mode : {"Classic", "Event"}) {
        testPlayback(directory, context, backend, mode);
        testDryAndMix(directory, context, backend, mode);
        testIdle(directory, context, backend, mode);
        testRepeatedOnsetAndRelease(directory, context, backend, mode);
      }
      testModeChangeAndRouting(directory, context, backend,
          backend == discovered.scalar.backend ? discovered.simd.backend : nullptr);
      testEventNoteHold(directory, context, backend);
      testEventPreviewStart(directory, context, backend);
      testTwoEventInstances(directory, context, backend);
      testSampleShiftAndPreview(shiftContext, backend,
          backend == discovered.scalar.backend ? discovered.simd.backend : nullptr);
    }
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; result = 1; }
  std::filesystem::remove_all(directory);
  return result;
}
