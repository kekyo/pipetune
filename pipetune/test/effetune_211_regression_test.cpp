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
#include <numbers>
#include <string>
#include <unistd.h>
#include <vector>

static bool check(bool condition, const std::string &message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static bool testOversampling(const std::filesystem::path &path) {
  struct Case { const char *name; const char *dry; const char *wet; bool exactDry; };
  const auto cases = std::vector<Case>{
      {"Saturation", R"("mx":0,"gn":0)", R"("dr":8,"bs":0,"mx":100,"gn":0)", true},
      {"Dynamic Saturation", R"("cm":0)", R"("sd":10,"dd":10,"cm":100,"dm":100)", true},
      {"Exciter", R"("mx":0)", R"("hs":0,"dr":8,"bs":0,"mx":100)", true},
      {"Hard Clipping", R"("th":0)", R"("th":-12)", false},
      {"Harmonic Distortion", R"("h2":0,"h3":0,"h4":0,"h5":0)", R"("h2":30,"h3":30,"h4":30,"h5":30)", false},
      {"Multiband Saturation", R"("bands":[{"mx":0},{"mx":0},{"mx":0}])",
       R"("bands":[{"dr":8,"bs":0},{"dr":8,"bs":0},{"dr":8,"bs":0}])", true}};
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &test : cases) {
    for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
      auto dryReference = std::vector<float>();
      auto defaultWet = std::vector<float>();
      for (const auto factor : {0u, 1u, 3u, 2u, 4u, 8u, 16u}) {
        if (factor == 16u && std::string_view(test.name) != "Hard Clipping") continue;
        const auto delay = factor == 0u || factor == 1u || factor == 3u ? 0u : 64u;
        auto dry = std::vector<float>();
        for (const auto wet : {false, true}) {
          {
            auto file = std::ofstream(path);
            file << "{\"pipeline\":[{\"name\":\"" << test.name << "\",\"channel\":\"All\",\"parameters\":{";
            if (factor != 0u) file << "\"os\":" << factor << ',';
            file << (wet ? test.wet : test.dry) << "}}]}";
          }
          auto loaded = pipetune::loadDspPipeline(path, {48000.0F, 2u, 257u}, backend);
          if (!check(loaded.pipeline != nullptr, loaded.error) ||
              !check(loaded.warnings.empty() && loaded.pipeline->latencyFrames() == delay,
                     std::string(test.name) + " OS latency differs")) return false;
          auto reference = std::vector<float>();
          for (auto repeat = 0u; repeat < (factor == 8u ? 3u : 1u); ++repeat) {
            if (repeat == 1u && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
            if (repeat == 2u) {
              auto highRate = pipetune::rebuildDspPipeline(*loaded.pipeline, {96000.0F, 2u, 257u},
                  backend == backends.scalar.backend ? backends.simd.backend : backends.scalar.backend);
              if (!check(highRate.pipeline != nullptr && highRate.pipeline->latencyFrames() == delay, highRate.error)) return false;
              auto probe = std::vector<float>(514, 0.2F);
              if (highRate.pipeline->process(probe, 2, 257, 0.0) != pipetune::ProcessStatus::ok) return false;
              loaded = pipetune::rebuildDspPipeline(*highRate.pipeline, {48000.0F, 2u, 257u}, backend);
              if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
            }
            // Complete the multiband startup fade before measuring its impulse.
            for (auto i = 0u; i < 4096u; i += 128u) {
              auto silence = std::vector<float>(256, 0.0F);
              if (loaded.pipeline->process(silence, 2, 128, i / 48000.0) != pipetune::ProcessStatus::ok) return false;
            }
            auto output = std::vector<float>();
            for (auto start = 0u; start < 4096u;) {
              const auto count = std::min(repeat == 1u ? 63u : 257u, 4096u - start);
              auto audio = std::vector<float>(2u * count);
              for (auto ch = 0u; ch < 2u; ++ch)
                for (auto i = 0u; i < count; ++i)
                  audio[ch * count + i] = wet ? static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 997.0 * (start + i) / 48000.0)) :
                      (start + i == 173u ? 0.05F : 0.0F);
              if (loaded.pipeline->process(audio, 2, count, (4096.0 + start) / 48000.0) != pipetune::ProcessStatus::ok) return false;
              output.insert(output.end(), audio.begin(), audio.begin() + count);
              start += count;
            }
            if (!check(std::ranges::all_of(output, [](float sample) { return std::isfinite(sample); }), "OS PCM must be finite")) return false;
            if (repeat == 0u) reference = output;
            else for (auto i = 0u; i < output.size(); ++i)
              if (!check(std::abs(output[i] - reference[i]) < 2.0e-5F,
                         std::string(test.name) + " must replay after reset/rate/backend rebuild")) return false;
          }
          if (!wet) {
            dry = reference;
            if (factor == 0u) dryReference = dry;
            for (auto i = delay; i < dry.size(); ++i)
              if (test.exactDry && !check(std::abs(dry[i] - dryReference[i - delay]) < 2.0e-6F,
                  std::string(test.name) + " dry path must align with OS delay")) return false;
            const auto peak = std::max_element(dry.begin(), dry.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
            const auto basePeak = std::max_element(dryReference.begin(), dryReference.end(), [](float a, float b) { return std::abs(a) < std::abs(b); });
            // Hard Clipping's legacy 1x interpolation/IIR path has its own
            // phase response. OS replaces it with FIRs centered on input + 64.
            const auto expectedPeak = delay != 0u && !test.exactDry ? 173u + delay :
                static_cast<unsigned>(basePeak - dryReference.begin()) + delay;
            if (!check(static_cast<unsigned>(peak - dry.begin()) == expectedPeak,
                       std::string(test.name) + " actual OS impulse peak differs: factor=" +
                       std::to_string(factor) + " peak=" + std::to_string(peak - dry.begin()) +
                       " base=" + std::to_string(basePeak - dryReference.begin()))) return false;
          } else {
            if (factor == 0u) defaultWet = reference;
            if ((factor == 1u || factor == 3u) && !check(reference == defaultWet,
                std::string(test.name) + " default and invalid factor 3 must equal 1x PCM")) return false;
            auto energy = 0.0;
            for (const auto sample : reference) energy += sample * sample;
            if (!check(energy > 0.01, std::string(test.name) + " wet processing must remain audible")) return false;
          }
        }
      }
    }
  }
  return true;
}

static bool testTimeAlignment(const std::filesystem::path &path) {
  const auto backends = pipetune::discoverDspBackends();
  for (const auto rate : {32000u, 48000u, 384000u}) {
    const auto width = rate == 384000u ? 16u : 2u;
    for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
      for (const auto delayMs : {0u, 500u, 501u}) {
        {
          auto file = std::ofstream(path);
          file << R"({"pipeline":[{"name":"Time Alignment","channel":"All","parameters":{"dl":)" << delayMs << "}}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path, {static_cast<float>(rate), width, 257u}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error) ||
            !check(loaded.pipeline->latencyFrames() == 0u, "intentional Time Alignment must not become host compensation")) return false;
        const auto delay = std::min(delayMs, 500u) * rate / 1000u;
        for (auto repeat = 0u; repeat < 2u; ++repeat) {
          if (repeat != 0u && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
          for (auto start = 0u; start < delay + 1024u;) {
            const auto count = std::min(repeat == 0u ? 257u : 63u, delay + 1024u - start);
            auto audio = std::vector<float>(width * count, 0.0F);
            if (start <= 17u && start + count > 17u)
              for (auto ch = 0u; ch < width; ++ch) audio[ch * count + 17u - start] = 0.05F * (ch + 1u);
            if (loaded.pipeline->process(audio, width, count, start / double(rate)) != pipetune::ProcessStatus::ok) return false;
            for (auto ch = 0u; ch < width; ++ch)
              for (auto i = 0u; i < count; ++i)
                if (!check(std::abs(audio[ch * count + i] - (start + i == delay + 17u ? 0.05F * (ch + 1u) : 0.0F)) < 1.0e-6F,
                           "Time Alignment must delay every impulse by up to 500 ms after reset")) return false;
            start += count;
          }
        }
      }
    }
  }
  return true;
}

static bool testLimiter(const std::filesystem::path &path) {
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto rate : {44100u, 96000u}) {
      for (const auto factor : {1u, 2u, 4u, 8u}) {
        for (const auto threshold : {0, -12}) {
          {
            auto file = std::ofstream(path);
            file << R"({"pipeline":[{"name":"Brickwall Limiter","channel":"All","parameters":{"la":3,"sm":0,"th":)"
                 << threshold << R"(,"os":)" << factor << "}}]}";
          }
          auto loaded = pipetune::loadDspPipeline(path, {static_cast<float>(rate), 2u, 257u}, backend);
          const auto delay = static_cast<unsigned>(std::ceil(rate * 0.003)) + (factor == 1u ? 0u : 64u);
          if (!check(loaded.pipeline != nullptr, loaded.error) ||
              !check(loaded.pipeline->latencyFrames() == delay, "Limiter must report lookahead plus 64 OS frames")) return false;
          const auto ceiling = std::pow(10.0F, threshold / 20.0F);
          // The legacy 1x reciprocal lookup rounds magnitudes down in steps
          // of 10/1024. Preserve upstream audio and bound that approximation;
          // OS paths instead clamp reconstructed samples to the exact ceiling.
          const auto upperLimit = factor == 1u ? ceiling / (1.0F - 10.0F / (1024.0F * ceiling)) : ceiling;
          for (const auto loud : {false, true}) {
            if (loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
            auto maximum = 0.0F;
            auto peakFrame = 0u;
            for (auto start = 0u; start < 16384u;) {
              const auto count = std::min(257u, 16384u - start);
              auto audio = std::vector<float>(2u * count);
              for (auto ch = 0u; ch < 2u; ++ch)
                for (auto i = 0u; i < count; ++i)
                  audio[ch * count + i] = loud ? static_cast<float>(2.0 * std::sin(2.0 * std::numbers::pi * 997.0 * (start + i) / rate)) :
                      (start + i == 173u ? 0.05F : 0.0F);
              if (loaded.pipeline->process(audio, 2, count, start / double(rate)) != pipetune::ProcessStatus::ok) return false;
              for (auto i = 0u; i < count; ++i) {
                if (!check(std::isfinite(audio[i]) && std::abs(audio[i]) <= upperLimit + 3.0e-5F,
                           "Limiter output must remain below its selected ceiling: rate=" + std::to_string(rate) +
                           " os=" + std::to_string(factor) + " th=" + std::to_string(threshold) +
                           " sample=" + std::to_string(audio[i]))) return false;
                if (std::abs(audio[i]) > maximum) { maximum = std::abs(audio[i]); peakFrame = start + i; }
              }
              start += count;
            }
            if (!check(maximum > (loud ? ceiling * 0.7F : 0.01F), "Limiter must preserve audible signal below and above threshold") ||
                (!loud && !check(peakFrame == 173u + delay, "Limiter actual impulse delay differs"))) return false;
            if (loud) std::cout << "limiter rate=" << rate << " os=" << factor << " threshold=" << threshold
                                << " peak=" << maximum << " bound=" << upperLimit << '\n';
          }
        }
      }
    }
  }
  return true;
}

static bool testOversamplingMixAndAliasing(const std::filesystem::path &path) {
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    auto legacyAlias = 0.0;
    for (const auto factor : {1u, 8u}) {
      auto renders = std::vector<std::vector<float>>();
      for (const auto mix : {0u, 50u, 100u}) {
        {
          auto file = std::ofstream(path);
          file << R"({"pipeline":[{"name":"Saturation","channel":"All","parameters":{"dr":8,"bs":0,"gn":0,"os":)"
               << factor << R"(,"mx":)" << mix << "}}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path, {48000.0F, 2u, 257u}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error)) return false;
        auto output = std::vector<float>();
        for (auto start = 0u; start < 96000u;) {
          const auto count = std::min(257u, 96000u - start);
          auto audio = std::vector<float>(2u * count);
          for (auto ch = 0u; ch < 2u; ++ch)
            for (auto i = 0u; i < count; ++i)
              audio[ch * count + i] = static_cast<float>(0.8 * std::sin(2.0 * std::numbers::pi * 18000.0 * (start + i) / 48000.0));
          if (loaded.pipeline->process(audio, 2, count, start / 48000.0) != pipetune::ProcessStatus::ok) return false;
          output.insert(output.end(), audio.begin(), audio.begin() + count);
          start += count;
        }
        renders.push_back(std::move(output));
      }
      for (auto i = 48000u; i < 96000u; ++i)
        if (!check(std::abs(renders[1][i] - (renders[0][i] + renders[2][i]) * 0.5F) < 2.0e-6F,
                   "50% mix must blend the aligned dry and wet paths")) return false;
      const auto amplitude = [&](double frequency) {
        auto real = 0.0;
        auto imaginary = 0.0;
        for (auto i = 48000u; i < 96000u; ++i) {
          const auto phase = 2.0 * std::numbers::pi * frequency * i / 48000.0;
          real += renders[2][i] * std::cos(phase);
          imaginary += renders[2][i] * std::sin(phase);
        }
        return 2.0 * std::hypot(real, imaginary) / 48000.0;
      };
      const auto alias = amplitude(6000.0);
      const auto fundamental = amplitude(18000.0);
      if (factor == 1u) legacyAlias = alias;
      std::cout << "saturation os=" << factor << " alias6000=" << alias << " fundamental18000=" << fundamental << '\n';
      // The third harmonic of 18 kHz folds to 6 kHz at 48 kHz. With 8x
      // processing reconstruction must suppress it by at least 20 dB.
      if (!check(fundamental > 0.1 && (factor == 1u ? alias > 0.01 : alias < legacyAlias * 0.1),
                 "OS must suppress the folded third harmonic while preserving the fundamental")) return false;
    }
  }
  return true;
}

static bool testMp3Recovery(const std::filesystem::path &path) {
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto rate : {48000u, 384000u}) {
      for (const auto mpeg2 : {false, true}) {
        {
          auto file = std::ofstream(path);
          file << R"({"pipeline":[{"name":"MP3 Codec Simulator","channel":"34","parameters":)"
               << (mpeg2 ? R"json({"cr":"22.05 kHz (MPEG-2)","br":"32","sm":"Stereo","rv":false})json" :
                           R"json({"cr":"44.1 kHz (MPEG-1)","br":"64","sm":"Joint Stereo","rv":true})json") << "}]}";
        }
        auto loaded = pipetune::loadDspPipeline(path, {static_cast<float>(rate), 4u, 257u}, backend);
        if (!check(loaded.pipeline != nullptr, loaded.error) ||
            !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1u, "MP3 must remain active")) return false;
        const auto delay = loaded.pipeline->latencyFrames();
        if (!check(delay > 0u, "MP3 must report codec latency")) return false;
        auto reference = std::vector<float>();
        const auto input = [rate](unsigned frame, unsigned ch) {
          const auto burst = frame % (rate / 5u) < rate / 10u ? 1.0 : 0.2;
          return static_cast<float>(burst * (0.2 * std::sin(2.0 * std::numbers::pi * (997.0 + ch * 113.0) * frame / rate) +
              0.08 * std::sin(2.0 * std::numbers::pi * 14000.0 * frame / rate)));
        };
        for (auto repeat = 0u; repeat < 3u; ++repeat) {
          if (repeat == 1u && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
          if (repeat == 2u) {
            auto alternate = pipetune::rebuildDspPipeline(*loaded.pipeline, {96000.0F, 4u, 257u},
                backend == backends.scalar.backend ? backends.simd.backend : backends.scalar.backend);
            if (!check(alternate.pipeline != nullptr, alternate.error)) return false;
            auto probe = std::vector<float>(1028, 0.1F);
            if (alternate.pipeline->process(probe, 4, 257, 0.0) != pipetune::ProcessStatus::ok) return false;
            loaded = pipetune::rebuildDspPipeline(*alternate.pipeline, {static_cast<float>(rate), 4u, 257u}, backend);
            if (!check(loaded.pipeline != nullptr && loaded.pipeline->latencyFrames() == delay, loaded.error)) return false;
          }
          auto energy = 0.0;
          auto difference = 0.0;
          for (auto start = 0u; start < rate + delay + 4097u;) {
            const auto count = std::min(repeat == 1u ? 63u : 257u, rate + delay + 4097u - start);
            auto audio = std::vector<float>(4u * count);
            for (auto ch = 0u; ch < 4u; ++ch)
              for (auto i = 0u; i < count; ++i) audio[ch * count + i] = input(start + i, ch);
            if (loaded.pipeline->process(audio, 4, count, start / double(rate)) != pipetune::ProcessStatus::ok) return false;
            for (auto i = 0u; i < count; ++i) {
              for (auto ch = 0u; ch < 4u; ++ch) {
                const auto sample = audio[ch * count + i];
                if (!check(std::isfinite(sample), "long MP3 output must stay finite")) return false;
                const auto dry = start + i < delay ? 0.0F : input(start + i - delay, ch);
                if (ch < 2u && !check(sample == dry, "MP3 must retain the unselected pair with latency compensation")) return false;
                if (ch >= 2u && start + i > delay + rate / 2u) {
                  energy += sample * sample;
                  difference += (sample - dry) * (sample - dry);
                }
              }
              if (repeat == 0u) reference.push_back(audio[2u * count + i]);
              else if (!check(std::abs(audio[2u * count + i] - reference[start + i]) < 2.0e-5F,
                              "MP3 must replay after reset and rate/backend rebuild")) return false;
            }
            start += count;
          }
          if (!check(energy > 1.0 && difference > 0.01, "MP3 must produce sustained, changed audio after startup")) return false;
        }
        std::cout << "mp3 rate=" << rate << " mpeg2=" << mpeg2 << " latency=" << delay << '\n';
      }
    }
  }
  return true;
}

static bool testMultiChannelPanel(const std::filesystem::path &path) {
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    for (const auto rate : {48000u, 384000u}) {
      {
        auto file = std::ofstream(path);
        file << R"({"pipeline":[{"name":"MultiChannel Panel","channel":"All","parameters":{"v":[-6,0,-3],"m":[false,true],"d":[0,0,1,0,0,0,0,0,0,0,0,0,0,0,0,30]}}]})";
      }
      auto loaded = pipetune::loadDspPipeline(path, {static_cast<float>(rate), 16u, 257u}, backend);
      if (!check(loaded.pipeline != nullptr, loaded.error) ||
          !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1u && loaded.pipeline->latencyFrames() == 0u,
                 "MultiChannel Panel must process audio without telemetry or compensation latency")) return false;
      for (auto repeat = 0u; repeat < 2u; ++repeat) {
        if (repeat != 0u && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return false;
        for (auto start = 0u; start < rate / 10u;) {
          const auto count = std::min(repeat == 0u ? 257u : 63u, rate / 10u - start);
          auto audio = std::vector<float>(16u * count, 0.0F);
          for (auto ch = 0u; ch < 16u; ++ch)
            if (start <= 17u && start + count > 17u) audio[ch * count + 17u - start] = 0.5F;
          if (loaded.pipeline->process(audio, 16, count, start / double(rate)) != pipetune::ProcessStatus::ok) return false;
          for (auto ch = 0u; ch < 16u; ++ch) {
            const auto delay = (ch == 2u ? 1u : ch == 15u ? 30u : 0u) * rate / 1000u;
            const auto gain = ch == 1u ? 0.0F : std::pow(10.0F, (ch == 0u ? -6.0F : ch == 2u ? -3.0F : 0.0F) / 20.0F);
            for (auto i = 0u; i < count; ++i)
              if (!check(std::abs(audio[ch * count + i] - (start + i == 17u + delay ? 0.5F * gain : 0.0F)) < 2.0e-6F,
                         "MultiChannel Panel must preserve per-channel gain, mute and delay after reset")) return false;
          }
          start += count;
        }
      }
    }
  }
  return true;
}

static bool testFmAudio(const std::filesystem::path &path) {
  {
    auto file = std::ofstream(path);
    file << R"({"pipeline":[{"name":"FM Radio Simulator","channel":"All","parameters":{"rd":true,"st":70,"mx":100}}]})";
  }
  const auto backends = pipetune::discoverDspBackends();
  for (const auto &backend : {backends.scalar.backend, backends.simd.backend}) {
    auto loaded = pipetune::loadDspPipeline(path, {48000.0F, 2u, 257u}, backend);
    if (!check(loaded.pipeline != nullptr, loaded.error) ||
        !check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1u,
               "FM Radio Simulator must remain an audio processor with telemetry disabled")) return false;
    auto energy = 0.0;
    auto difference = 0.0;
    for (auto start = 0u; start < 48000u;) {
      const auto count = std::min(257u, 48000u - start);
      auto audio = std::vector<float>(2u * count);
      for (auto ch = 0u; ch < 2u; ++ch)
        for (auto i = 0u; i < count; ++i)
          audio[ch * count + i] = 0.2F * std::sin(static_cast<float>(start + i) * 0.05F);
      const auto input = audio;
      if (loaded.pipeline->process(audio, 2, count, start / 48000.0) != pipetune::ProcessStatus::ok) return false;
      for (auto i = 0u; i < audio.size(); ++i) {
        energy += audio[i] * audio[i];
        difference += (audio[i] - input[i]) * (audio[i] - input[i]);
      }
      start += count;
    }
    if (!check(std::isfinite(energy) && energy > 1.0 && difference > 0.01,
               "FM must preserve audible processed output")) return false;
  }
  return true;
}

int main() {
  const auto path = std::filesystem::temp_directory_path() /
      ("pipetune-211-regressions-" + std::to_string(getpid()) + ".effetune_preset");
  const auto passed = testOversampling(path) && testOversamplingMixAndAliasing(path) &&
      testTimeAlignment(path) && testLimiter(path) && testMp3Recovery(path) && testMultiChannelPanel(path) && testFmAudio(path);
  std::filesystem::remove(path);
  return passed ? 0 : 1;
}
