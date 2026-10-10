/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "phase.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace pipetune::assets::room_eq {

static std::vector<double> predictedResponse(const DirectSpectrum &direct,
    std::span<const double> magnitudes, std::span<const double> phase) {
  auto spectrum = Spectrum{std::vector<double>(magnitudes.size()), std::vector<double>(magnitudes.size())};
  for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
    const auto magnitude = direct.magnitude[bin] * magnitudes[bin];
    const auto angle = direct.phase[bin] + phase[bin];
    spectrum.real[bin] = magnitude * std::cos(angle);
    spectrum.imag[bin] = magnitude * std::sin(angle);
  }
  spectrum.imag.front() = 0; spectrum.imag.back() = 0;
  return inverseRealTransform(spectrum);
}

static std::size_t wrappedIndex(std::int64_t index, std::size_t size) {
  const auto remainder = index % static_cast<std::int64_t>(size);
  return remainder < 0 ? remainder + size : remainder;
}

static double localEnergy(std::span<const double> samples, std::size_t center, std::span<const double> weights) {
  const auto radius = static_cast<std::int64_t>((weights.size() - 1) / 2);
  auto index = wrappedIndex(static_cast<std::int64_t>(center) - radius, samples.size());
  auto energy = 0.0;
  for (const auto weight : weights) {
    const auto value = samples[index];
    energy += value * value * weight;
    if (++index == samples.size()) index = 0;
  }
  return energy;
}

static double dominantEnergyPosition(std::span<const double> samples, std::span<const double> weights,
    std::optional<double> center, std::size_t searchRadius) {
  const auto centerIndex = center ? static_cast<std::int64_t>(std::floor(*center + 0.5)) : 0;
  const auto count = center ? searchRadius * 2 + 1 : samples.size();
  const auto radius = static_cast<std::int64_t>((weights.size() - 1) / 2);
  const auto angle = std::numbers::pi / (radius + 1), angleCosine = std::cos(angle), angleSine = std::sin(angle);
  const auto edgeCosine = std::cos(angle * radius), edgeSine = std::sin(angle * radius);
  auto index = wrappedIndex(centerIndex - (center ? static_cast<std::int64_t>(searchRadius) : 0), samples.size());
  auto rectangular = 0.0, cosine = 0.0, sine = 0.0;
  for (auto offset = -radius; offset <= radius; ++offset) {
    const auto sample = samples[wrappedIndex(static_cast<std::int64_t>(index) + offset, samples.size())];
    const auto energy = sample * sample;
    rectangular += energy; cosine += energy * std::cos(angle * offset); sine += energy * std::sin(angle * offset);
  }
  auto bestIndex = index;
  auto bestEnergy = -1.0, bestDistance = std::numeric_limits<double>::infinity();
  // Advance the Hann window's rectangular/cosine/sine sums in O(samples), then
  // recompute the winning neighborhood for stable sub-sample interpolation.
  for (auto step = std::size_t{0}; step < count; ++step) {
    const auto offset = static_cast<std::int64_t>(step) - (center ? static_cast<std::int64_t>(searchRadius) : 0);
    const auto energy = 0.5 * (rectangular + cosine), distance = static_cast<double>(std::abs(offset));
    if (energy > bestEnergy || (energy == bestEnergy && distance < bestDistance)) {
      bestIndex = index; bestEnergy = energy; bestDistance = distance;
    }
    if (step + 1 == count) break;
    const auto left = samples[wrappedIndex(static_cast<std::int64_t>(index) - radius, samples.size())];
    const auto right = samples[wrappedIndex(static_cast<std::int64_t>(index) + radius + 1, samples.size())];
    const auto leftEnergy = left * left, rightEnergy = right * right;
    rectangular += rightEnergy - leftEnergy;
    const auto sharedCosine = cosine - leftEnergy * edgeCosine, sharedSine = sine + leftEnergy * edgeSine;
    cosine = angleCosine * sharedCosine + angleSine * sharedSine + rightEnergy * edgeCosine;
    sine = angleCosine * sharedSine - angleSine * sharedCosine + rightEnergy * edgeSine;
    if (++index == samples.size()) index = 0;
  }
  bestEnergy = localEnergy(samples, bestIndex, weights);
  const auto left = localEnergy(samples, bestIndex ? bestIndex - 1 : samples.size() - 1, weights);
  const auto right = localEnergy(samples, bestIndex + 1 == samples.size() ? 0 : bestIndex + 1, weights);
  const auto denominator = left - 2 * bestEnergy + right;
  const auto fraction = denominator < 0 ? std::clamp(0.5 * (left - right) / denominator, -0.5, 0.5) : 0;
  return bestIndex + (std::isfinite(fraction) ? fraction : 0);
}

double timingAlignment(const DirectSpectrum &direct, std::span<const double> magnitudes,
    std::span<const double> referencePhase, std::span<const double> phase,
    const RoomEqConfiguration &config, double searchWindowMs) {
  const auto radius = static_cast<std::size_t>(std::max(1.0, std::round(config.sampleRate * 0.000125)));
  auto weights = std::vector<double>(radius * 2 + 1);
  for (auto index = std::size_t{0}; index < weights.size(); ++index)
    weights[index] = 0.5 + 0.5 * std::cos(std::numbers::pi * (static_cast<double>(index) - radius) / (radius + 1));
  const auto reference = predictedResponse(direct, magnitudes, referencePhase);
  const auto position = dominantEnergyPosition(reference, weights, std::nullopt, 0);
  const auto corrected = predictedResponse(direct, magnitudes, phase);
  const auto searchRadius = static_cast<std::size_t>(std::min(config.taps / 2.0 - 1, std::round(config.sampleRate * searchWindowMs / 1000)));
  auto offset = dominantEnergyPosition(corrected, weights, position, searchRadius) - position;
  const auto fftSize = (magnitudes.size() - 1) * 2;
  if (offset > fftSize / 2.0) offset -= fftSize;
  else if (offset < -static_cast<double>(fftSize) / 2) offset += fftSize;
  return std::isfinite(offset) ? offset / config.sampleRate : 0;
}

} // namespace pipetune::assets::room_eq
