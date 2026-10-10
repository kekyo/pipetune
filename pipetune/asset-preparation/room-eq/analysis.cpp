/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js: analyzeImpulse and
 * reduceSpectrumToLogGrid. Copyright (c) 2025-2026 Yoshiyuki Kobayashi.
 * MIT; see LICENSE.effetune.
 */
#include "analysis.h"
#include "fft.h"
#include "common/resample.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace pipetune::assets::room_eq {

ImpulseAnalysis analyzeImpulse(const RoomEqImpulse &impulse, std::uint32_t sampleRate,
                               std::span<const double> frequencies) {
  if (!impulse.sampleRate || impulse.sampleRate > 768000 || !sampleRate || frequencies.size() < 2 ||
      impulse.samples.empty() || impulse.onsetIndex >= impulse.samples.size())
    throw std::invalid_argument("invalid Room EQ impulse metadata");
  for (const auto sample : impulse.samples)
    if (!std::isfinite(sample)) throw std::invalid_argument("Room EQ impulse contains nonfinite PCM");
  auto result = ImpulseAnalysis{};
  result.samples = resampleMeasurement(impulse.samples, impulse.sampleRate, sampleRate, 0);
  const auto scale = std::isfinite(impulse.referenceScale) && impulse.referenceScale > 1e-8 ? impulse.referenceScale : 1;
  if (scale != 1) for (auto &sample : result.samples) sample = static_cast<float>(sample / scale);
  for (const auto sample : result.samples)
    if (!std::isfinite(sample)) throw std::invalid_argument("Room EQ scaled impulse is nonfinite");
  result.onsetIndex = static_cast<std::size_t>(std::llround(static_cast<double>(impulse.onsetIndex) * sampleRate / impulse.sampleRate));
  result.fftSize = std::bit_ceil(result.samples.size());
  auto input = std::vector<double>(result.fftSize);
  std::copy(result.samples.begin(), result.samples.end(), input.begin());
  const auto spectrum = realTransform(input);
  result.spectrumMagnitude.resize(spectrum.real.size());
  for (auto bin = std::size_t{0}; bin < spectrum.real.size(); ++bin)
    result.spectrumMagnitude[bin] = std::hypot(spectrum.real[bin], spectrum.imag[bin]);
  result.magnitude.resize(frequencies.size());
  const auto binWidth = static_cast<double>(sampleRate) / result.fftSize;
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    const auto lower = index == 0 ? frequencies[index] / std::sqrt(frequencies[1] / frequencies[0]) :
                                   std::sqrt(frequencies[index - 1] * frequencies[index]);
    const auto upper = index + 1 == frequencies.size() ? frequencies[index] * std::sqrt(frequencies[index] / frequencies[index - 1]) :
                                                       std::sqrt(frequencies[index] * frequencies[index + 1]);
    auto first = std::max<std::size_t>(1, static_cast<std::size_t>(std::ceil(lower / binWidth)));
    auto last = std::min(spectrum.real.size() - 1, static_cast<std::size_t>(std::floor(upper / binWidth)));
    if (last < first) first = last = std::clamp<std::size_t>(std::llround(frequencies[index] / binWidth), 1, spectrum.real.size() - 1);
    auto power = 0.0;
    for (auto bin = first; bin <= last; ++bin)
      power += spectrum.real[bin] * spectrum.real[bin] + spectrum.imag[bin] * spectrum.imag[bin];
    result.magnitude[index] = std::sqrt(power / (last - first + 1));
  }
  return result;
}

} // namespace pipetune::assets::room_eq
