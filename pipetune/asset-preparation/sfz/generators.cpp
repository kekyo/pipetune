/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from EffeTune v2.17.0 js/sfz/generators.js.
 * https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/generators.js
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "generators.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

namespace pipetune::assets {

bool isSfzGenerator(std::string_view sample) noexcept {
  constexpr auto generators = std::array<std::string_view, 7>{
      "*sine", "*saw", "*square", "*triangle", "*tri", "*noise", "*silence"};
  return std::ranges::find(generators, sample) != generators.end();
}

SfzSampleMetadata sfzGeneratorMetadata(std::string_view sample) {
  if (!isSfzGenerator(sample)) throw SfzError("prepare", "Unknown SFZ sample generator.");
  return {std::string(sample), 0, sample == "*noise" || sample == "*silence" ? 2u : 2048u, 1u};
}

Audio createSfzGeneratorSample(std::string_view sample) {
  const auto metadata = sfzGeneratorMetadata(sample);
  const auto rate = static_cast<std::uint32_t>(std::round(440 * std::pow(2.0, (60.0 - 69) / 12) * 2048));
  auto audio = Audio{rate, {std::vector<float>(metadata.frames)}};
  for (auto frame = std::size_t{0}; frame < metadata.frames; ++frame) {
    const auto phase = static_cast<double>(frame) / static_cast<double>(metadata.frames);
    auto value = 0.0;
    if (sample == "*sine") value = std::sin(2 * std::numbers::pi * phase);
    else if (sample == "*saw") value = 2 * phase - 1;
    else if (sample == "*square") value = phase < .5 ? 1 : -1;
    else if (sample == "*triangle" || sample == "*tri") value = 1 - 4 * (phase < .5 ? .5 - phase : phase - .5);
    // Native per-voice RNG produces continuous noise. Noise and silence only
    // need two silent storage frames to keep the sample descriptor valid.
    audio.channels[0][frame] = static_cast<float>(value);
  }
  return audio;
}

} // namespace pipetune::assets
