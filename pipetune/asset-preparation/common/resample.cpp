/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/utils/measurement-dsp/resample.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see ../room-eq/LICENSE.effetune.
 */
#include "resample.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>
#include <stdexcept>

namespace pipetune::assets {

static double bessel(double value) {
  auto sum = 1.0, term = 1.0;
  const auto scaled = value * value / 4;
  for (auto index = 1; index < 20; ++index) {
    term *= scaled / (index * index);
    sum += term;
    if (term < sum * 1e-12) break;
  }
  return sum;
}

struct PhaseWeights {
  std::uint32_t phase = UINT32_MAX;
  std::vector<double> weights;
};

std::vector<float> resampleMeasurement(std::span<const float> input, std::uint32_t sourceRate,
                                     std::uint32_t targetRate, std::uint32_t radius) {
  if (input.empty() || !sourceRate || !targetRate) throw std::invalid_argument("invalid measurement resampling input");
  if (sourceRate == targetRate) return {input.begin(), input.end()};
  const auto divisor = std::gcd(sourceRate, targetRate);
  const auto sourceStep = sourceRate / divisor, phaseCount = targetRate / divisor;
  const auto bandLimit = std::min(1.0, static_cast<double>(targetRate) / sourceRate);
  const auto cutoff = bandLimit * 0.95;
  constexpr auto beta = 0.1102 * (100 - 8.7);
  const auto normalizer = bessel(beta);
  if (!radius) radius = static_cast<std::uint32_t>(std::ceil(92 / (4.57 * std::numbers::pi * bandLimit * 0.1)));
  // Coprime rates can have hundreds of thousands of distinct phases. Keep a
  // bounded preparation-local cache; collisions change cost, never coefficients.
  const auto bytesPerPhase = static_cast<std::uint64_t>(radius) * 2 * sizeof(double) + sizeof(PhaseWeights);
  const auto slots = std::min<std::uint64_t>(phaseCount, std::max<std::uint64_t>(1, 4u * 1024u * 1024u / bytesPerPhase));
  auto phases = std::vector<PhaseWeights>(slots);
  auto output = std::vector<float>(std::max<std::size_t>(1,
      std::llround(static_cast<double>(input.size()) * targetRate / sourceRate)));
  for (auto index = std::size_t{0}; index < output.size(); ++index) {
    const auto center = static_cast<std::int64_t>(index * sourceStep / phaseCount);
    const auto phase = index * sourceStep % phaseCount;
    auto &entry = phases[phase % slots];
    auto &weights = entry.weights;
    if (entry.phase != phase) {
      entry.phase = phase;
      weights.assign(2u * radius, 0);
      auto total = 0.0;
      for (auto tap = std::size_t{0}; tap < weights.size(); ++tap) {
        const auto distance = static_cast<double>(phase) / phaseCount - (static_cast<double>(tap) - radius + 1);
        const auto normalized = distance / radius;
        if (normalized <= -1 || normalized >= 1) continue;
        const auto window = bessel(beta * std::sqrt(1 - normalized * normalized)) / normalizer;
        const auto x = std::numbers::pi * (distance * cutoff);
        weights[tap] = cutoff * (x == 0 ? 1 : std::sin(x) / x) * window;
        total += weights[tap];
      }
      if (total != 0) for (auto &weight : weights) weight /= total;
    }
    const auto first = center - radius + 1;
    auto weighted = 0.0, total = 0.0;
    for (auto tap = std::size_t{0}; tap < weights.size(); ++tap) {
      const auto source = first + static_cast<std::int64_t>(tap);
      if (source < 0 || source >= static_cast<std::int64_t>(input.size())) continue;
      weighted += input[source] * weights[tap];
      total += weights[tap];
    }
    const auto interior = first >= 0 && static_cast<std::uint64_t>(first) + weights.size() <= input.size();
    output[index] = static_cast<float>(interior ? weighted : total == 0 ? 0 : weighted / total);
  }
  return output;
}

} // namespace pipetune::assets
