/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js and
 * js/utils/measurement-dsp/smoothing.js. Copyright (c) 2025-2026
 * Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "numeric.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace pipetune::assets::room_eq {

std::vector<double> logGrid(double low, double high) {
  const auto octaves = std::log2(high / low);
  const auto steps = static_cast<std::size_t>(std::ceil(octaves / 0.01));
  auto result = std::vector<double>(steps + 1);
  for (auto index = std::size_t{0}; index <= steps; ++index)
    result[index] = low * std::pow(2, static_cast<double>(index) / steps * octaves);
  return result;
}

std::vector<double> interpolate(std::span<const double> frequencies,
                                       std::span<const double> values,
                                       std::span<const double> targets) {
  auto output = std::vector<double>(targets.size());
  auto upper = std::size_t{1};
  for (auto index = std::size_t{0}; index < targets.size(); ++index) {
    const auto frequency = targets[index];
    while (upper < frequencies.size() && frequencies[upper] < frequency) ++upper;
    if (frequency <= frequencies.front()) output[index] = values.front();
    else if (upper >= frequencies.size()) output[index] = values.back();
    else {
      const auto fraction = std::log(frequency / frequencies[upper - 1]) /
                            std::log(frequencies[upper] / frequencies[upper - 1]);
      output[index] = values[upper - 1] + fraction * (values[upper] - values[upper - 1]);
    }
  }
  return output;
}

std::vector<double> smooth(std::span<const double> frequencies,
                                  std::span<const double> values, double sigma) {
  if (values.size() < 3 || sigma <= 0) return {values.begin(), values.end()};
  auto logs = std::vector<double>(frequencies.size());
  for (auto index = std::size_t{0}; index < logs.size(); ++index) logs[index] = std::log2(frequencies[index]);
  const auto spacing = (logs.back() - logs.front()) / (logs.size() - 1);
  auto uniform = std::isfinite(spacing) && spacing > 0;
  for (auto index = std::size_t{1}; uniform && index + 1 < logs.size(); ++index)
    uniform = std::abs(logs[index] - (logs.front() + index * spacing)) <= 1e-10;
  auto weights = std::vector<double>(uniform ? values.size() : 0);
  const auto denominator = 2 * sigma * sigma;
  auto radius = values.size() - 1;
  if (uniform) {
    for (auto offset = std::size_t{0}; offset < weights.size(); ++offset) {
      const auto distance = offset * spacing;
      weights[offset] = std::exp(-(distance * distance) / denominator);
    }
    constexpr auto epsilon = std::numeric_limits<double>::epsilon();
    while (radius > 0 && weights[radius] <= epsilon * epsilon) --radius;
  }
  auto output = std::vector<double>(values.size());
  for (auto index = std::size_t{0}; index < values.size(); ++index) {
    auto weighted = 0.0, total = 0.0;
    const auto first = index > radius ? index - radius : 0;
    const auto last = std::min(values.size(), index + radius + 1);
    for (auto candidate = first; candidate < last; ++candidate) {
      const auto distance = logs[candidate] - logs[index];
      const auto weight = uniform ? weights[candidate > index ? candidate - index : index - candidate]
          : std::exp(-(distance * distance) / denominator);
      weighted += values[candidate] * weight;
      total += weight;
    }
    output[index] = weighted / total;
  }
  return output;
}

std::vector<double> minimumPhase(std::span<const double> magnitudes) {
  auto spectrum = Spectrum{std::vector<double>(magnitudes.size()), std::vector<double>(magnitudes.size())};
  for (auto index = std::size_t{0}; index < magnitudes.size(); ++index)
    spectrum.real[index] = std::log(std::max(1e-8, magnitudes[index]));
  auto cepstrum = inverseRealTransform(spectrum);
  const auto half = cepstrum.size() / 2;
  for (auto index = std::size_t{1}; index < half; ++index) cepstrum[index] *= 2;
  std::fill(cepstrum.begin() + half + 1, cepstrum.end(), 0);
  return realTransform(cepstrum).imag;
}

RenderedFilter renderSynthesis(std::span<const double> magnitudes,
    std::span<const double> phase, const RoomEqConfiguration &config) {
  const auto fftSize = config.taps * 2;
  auto spectrum = Spectrum{std::vector<double>(magnitudes.size()), std::vector<double>(magnitudes.size())};
  if (config.phase == RoomEqPhase::Linear) {
    for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
      if ((bin & 3) == 0) spectrum.real[bin] = magnitudes[bin];
      else if ((bin & 3) == 1) spectrum.imag[bin] = -magnitudes[bin];
      else if ((bin & 3) == 2) spectrum.real[bin] = -magnitudes[bin];
      else spectrum.imag[bin] = magnitudes[bin];
    }
  } else {
    for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
      spectrum.real[bin] = magnitudes[bin] * std::cos(phase[bin]);
      spectrum.imag[bin] = magnitudes[bin] * std::sin(phase[bin]);
    }
  }
  spectrum.imag.front() = spectrum.imag.back() = 0;
  auto time = inverseRealTransform(spectrum);
  auto taps = std::vector<float>(config.taps);
  const auto edge = config.taps * 0.05;
  const auto fadeStart = static_cast<std::size_t>(std::floor(config.taps * 0.9));
  for (auto index = std::size_t{0}; index < taps.size(); ++index) {
    auto window = 1.0;
    if (config.phase == RoomEqPhase::Minimum) {
      if (index >= fadeStart) window = 0.5 + 0.5 * std::cos(std::numbers::pi *
          (index - fadeStart) / std::max<std::size_t>(1, config.taps - fadeStart - 1));
    } else if (index < edge) window = 0.5 - 0.5 * std::cos(std::numbers::pi * index / edge);
    else if (index > config.taps - edge) window = 0.5 - 0.5 * std::cos(std::numbers::pi * (config.taps - index) / edge);
    taps[index] = static_cast<float>(time[index] * window);
    if (!std::isfinite(taps[index])) throw std::invalid_argument("Room EQ design produced nonfinite coefficients");
    time[index] = taps[index];
  }
  std::fill(time.begin() + config.taps, time.end(), 0);
  auto actual = realTransform(time);
  auto maximumMagnitudeError = 0.0, minimumPhaseCosine = 1.0;
  for (auto bin = std::size_t{1}; bin < magnitudes.size(); ++bin) {
    const auto frequency = static_cast<double>(bin) * config.sampleRate / fftSize;
    if (frequency < config.lowFrequency || frequency > std::min(config.highFrequency, config.sampleRate * 0.45)) continue;
    const auto real = actual.real[bin], imag = actual.imag[bin];
    const auto power = real * real + imag * imag, intended = magnitudes[bin] * magnitudes[bin];
    maximumMagnitudeError = std::max(maximumMagnitudeError, std::abs(10 * std::log10(std::max(1e-16, power) / intended)));
    const auto denominator = std::sqrt(power * intended);
    if (config.phase != RoomEqPhase::Minimum && denominator > 1e-16)
      minimumPhaseCosine = std::min(minimumPhaseCosine,
          (real * spectrum.real[bin] + imag * spectrum.imag[bin]) / denominator);
  }
  const auto phaseError = std::acos(std::clamp(minimumPhaseCosine, -1.0, 1.0));
  return {std::move(taps), std::move(actual), maximumMagnitudeError, phaseError,
          maximumMagnitudeError > 0.5 || phaseError > 0.05};
}

} // namespace pipetune::assets::room_eq
