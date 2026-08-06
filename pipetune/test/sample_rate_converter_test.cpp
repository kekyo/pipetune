#include "sample_rate_converter.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <numbers>
#include <string_view>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testIndependentFixedDspRate() {
  const auto suggested = pipetune::resolveSampleRateBridgeRates(
      {.mode = pipetune::SampleRateMode::fixed,
       .fixedRate = 192000,
       .enforcement = pipetune::SampleRateEnforcement::suggest},
      48000);
  const auto forced = pipetune::resolveSampleRateBridgeRates(
      {.mode = pipetune::SampleRateMode::fixed,
       .fixedRate = 192000,
       .enforcement = pipetune::SampleRateEnforcement::force},
      48000);
  const auto automatic = pipetune::resolveSampleRateBridgeRates(
      pipetune::defaultSampleRatePolicy(), 48000);
  const auto unavailable = pipetune::resolveSampleRateBridgeRates(
      pipetune::defaultSampleRatePolicy(), 0);
  return check(suggested.streamSampleRate == 48000 &&
                   suggested.dspSampleRate == 192000 &&
                   suggested.conversionRequired,
               "Suggest must bridge graph PCM into the fixed DSP rate") &&
         check(forced.streamSampleRate == 48000 &&
                   forced.dspSampleRate == 192000 &&
                   forced.conversionRequired,
               "Force startup must bridge PCM until the graph rate changes") &&
         check(automatic.streamSampleRate == 48000 &&
                   automatic.dspSampleRate == 48000 &&
                   !automatic.conversionRequired,
               "automatic mode must process at the negotiated PCM rate") &&
         check(unavailable.streamSampleRate == 0 &&
                   unavailable.dspSampleRate == 0 &&
                   !unavailable.conversionRequired,
               "an unavailable stream rate must not produce a bridge");
}

static bool testEqualRatesPassThroughExactly() {
  auto converter = pipetune::PlanarSampleRateConverter(2, 8);
  const auto input = std::vector<float>{1.0F, 2.0F, 3.0F, 4.0F,
                                        11.0F, 12.0F, 13.0F, 14.0F};
  auto output = std::vector<float>(8, -1.0F);
  const auto configured = converter.configure(48000, 48000);
  const auto converted = converter.process(input, 4, 1, 2, output, 4);
  return check(configured == 0, "equal rates must be configurable") &&
         check(converted.error == 0 && converted.inputFramesUsed == 2 &&
                   converted.outputFramesGenerated == 2,
               "equal-rate conversion counts differ") &&
         check(output[0] == 2.0F && output[1] == 3.0F &&
                   output[2] == 12.0F && output[3] == 13.0F,
               "equal-rate PCM must pass through exactly");
}

static bool testStreamingUpsamplingPreservesSignal() {
  constexpr auto inputFrames = std::uint32_t{1024};
  constexpr auto outputCapacity = std::uint32_t{4096};
  auto converter = pipetune::PlanarSampleRateConverter(2, outputCapacity);
  if (!check(converter.configure(48000, 192000) == 0,
             "48-to-192 kHz conversion must be configurable")) {
    return false;
  }

  auto input = std::vector<float>(2 * inputFrames, 0.0F);
  for (auto frame = std::uint32_t{0}; frame < inputFrames; ++frame) {
    const auto phase = 2.0 * std::numbers::pi * 1000.0 * frame / 48000.0;
    input[frame] = static_cast<float>(0.5 * std::sin(phase));
    input[inputFrames + frame] = -input[frame];
  }
  auto output = std::vector<float>(2 * outputCapacity, 0.0F);
  auto offset = std::uint32_t{0};
  auto generated = std::uint32_t{0};
  while (offset < inputFrames) {
    const auto converted = converter.process(
        input, inputFrames, offset, inputFrames - offset, output,
        outputCapacity);
    if (!check(converted.error == 0,
               "streaming conversion returned an error") ||
        !check(converted.inputFramesUsed != 0 ||
                   converted.outputFramesGenerated != 0,
               "streaming conversion made no progress")) {
      return false;
    }
    offset += converted.inputFramesUsed;
    generated += converted.outputFramesGenerated;
    for (auto sample = std::size_t{0};
         sample < static_cast<std::size_t>(2) *
                      converted.outputFramesGenerated;
         ++sample) {
      if (!check(std::isfinite(output[sample]) &&
                     std::abs(output[sample]) <= 0.6F,
                 "upsampled PCM must remain finite and bounded")) {
        return false;
      }
    }
  }
  return check(generated > 3000 && generated <= outputCapacity,
               "upsampled duration is outside the streaming filter latency");
}

static bool testStreamingDownsamplingPreservesSignal() {
  constexpr auto inputFrames = std::uint32_t{4096};
  constexpr auto outputCapacity = std::uint32_t{1024};
  auto converter = pipetune::PlanarSampleRateConverter(2, inputFrames);
  if (!check(converter.configure(192000, 48000) == 0,
             "192-to-48 kHz conversion must be configurable")) {
    return false;
  }

  auto input = std::vector<float>(2 * inputFrames, 0.0F);
  for (auto frame = std::uint32_t{0}; frame < inputFrames; ++frame) {
    const auto phase = 2.0 * std::numbers::pi * 1000.0 * frame / 192000.0;
    input[frame] = static_cast<float>(0.5 * std::sin(phase));
    input[inputFrames + frame] = -input[frame];
  }
  auto output = std::vector<float>(2 * outputCapacity, 0.0F);
  const auto converted = converter.process(
      input, inputFrames, 0, inputFrames, output, outputCapacity);
  if (!check(converted.error == 0,
             "downsampling returned an error") ||
      !check(converted.inputFramesUsed > 3500 &&
                 converted.outputFramesGenerated > 800 &&
                 converted.outputFramesGenerated <= outputCapacity,
             "downsampled duration is outside the streaming filter latency")) {
    return false;
  }
  auto peak = 0.0F;
  for (auto sample = std::size_t{0};
       sample < static_cast<std::size_t>(2) *
                    converted.outputFramesGenerated;
       ++sample) {
    if (!check(std::isfinite(output[sample]) &&
                   std::abs(output[sample]) <= 0.6F,
               "downsampled PCM must remain finite and bounded")) {
      return false;
    }
    peak = std::max(peak, std::abs(output[sample]));
  }
  return check(peak > 0.4F,
               "downsampling must preserve the audible signal");
}

int main() {
  return testIndependentFixedDspRate() && testEqualRatesPassThroughExactly() &&
                 testStreamingUpsamplingPreservesSignal() &&
                 testStreamingDownsamplingPreservesSignal()
             ? 0
             : 1;
}
