/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/group-delay-analysis.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "group_delay.h"
#include "numeric.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace pipetune::assets::room_eq {

static constexpr auto missing = std::numeric_limits<double>::quiet_NaN();

static std::vector<double> interpolateLinear(std::span<const double> values,
    std::uint32_t sampleRate, std::size_t fftSize, std::span<const double> frequencies,
    std::span<const std::uint8_t> valid, std::vector<std::uint8_t> &outputValid) {
  auto output = std::vector<double>(frequencies.size(), missing);
  outputValid.assign(frequencies.size(), 0);
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    const auto position = frequencies[index] * fftSize / sampleRate;
    const auto lower = static_cast<std::size_t>(std::clamp(std::floor(position), 0.0, static_cast<double>(values.size() - 1)));
    const auto upper = std::min(values.size() - 1, lower + 1);
    const auto fraction = upper == lower ? 0 : position - lower;
    const auto exactLower = std::abs(fraction) < 1e-10, exactUpper = std::abs(1 - fraction) < 1e-10;
    if (exactUpper) {
      if (!valid[upper] || !std::isfinite(values[upper])) continue;
      output[index] = values[upper];
    } else {
      if (!valid[lower] || !std::isfinite(values[lower]) ||
          (!exactLower && (!valid[upper] || !std::isfinite(values[upper])))) continue;
      output[index] = exactLower ? values[lower] : values[lower] + fraction * (values[upper] - values[lower]);
    }
    outputValid[index] = 1;
  }
  return output;
}

static std::vector<double> minimumDelaySamples(std::span<const double> magnitudes, double floor) {
  auto logarithm = Spectrum{std::vector<double>(magnitudes.size()), std::vector<double>(magnitudes.size())};
  for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin)
    logarithm.real[bin] = std::log(magnitudes[bin] > floor ? magnitudes[bin] : floor);
  auto cepstrum = inverseRealTransform(logarithm);
  const auto half = cepstrum.size() / 2;
  for (auto index = std::size_t{1}; index < half; ++index) cepstrum[index] *= 2;
  std::fill(cepstrum.begin() + half + 1, cepstrum.end(), 0);
  for (auto index = std::size_t{0}; index < cepstrum.size(); ++index) cepstrum[index] *= index;
  return realTransform(cepstrum).real;
}

GroupDelay analyzeGroupDelay(std::span<const double> samples, double alignmentSamples,
    std::uint32_t sampleRate, std::span<const double> frequencies, std::size_t minimumFftSize) {
  auto result = GroupDelay{};
  result.fftSize = std::bit_ceil(std::max({samples.size() * 4, minimumFftSize, std::size_t{4}}));
  auto input = std::vector<double>(result.fftSize);
  std::copy(samples.begin(), samples.end(), input.begin());
  result.spectrum = realTransform(input);
  for (auto index = std::size_t{0}; index < samples.size(); ++index) input[index] = samples[index] * index;
  const auto ramped = realTransform(input);
  auto magnitudes = std::vector<double>(result.spectrum.real.size());
  auto peak = 0.0;
  for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
    magnitudes[bin] = std::hypot(result.spectrum.real[bin], result.spectrum.imag[bin]);
    peak = std::max(peak, magnitudes[bin]);
  }
  const auto floor = std::max(std::numeric_limits<double>::denorm_min(), peak * 1e-6);
  auto valid = std::vector<std::uint8_t>(magnitudes.size());
  auto totalSamples = std::vector<double>(magnitudes.size(), missing);
  for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
    if (!(magnitudes[bin] > floor)) continue;
    const auto real = result.spectrum.real[bin], imag = result.spectrum.imag[bin];
    const auto power = real * real + imag * imag;
    if (!(power > 0)) continue;
    valid[bin] = 1;
    totalSamples[bin] = (ramped.real[bin] * real + ramped.imag[bin] * imag) / power - alignmentSamples;
  }
  const auto minimumSamples = minimumDelaySamples(magnitudes, floor);
  auto totalValid = std::vector<std::uint8_t>{}, minimumValid = std::vector<std::uint8_t>{};
  const auto totalGrid = interpolateLinear(totalSamples, sampleRate, result.fftSize, frequencies, valid, totalValid);
  const auto minimumGrid = interpolateLinear(minimumSamples, sampleRate, result.fftSize, frequencies, valid, minimumValid);
  result.valid.resize(frequencies.size());
  result.totalMs.assign(frequencies.size(), missing);
  result.minimumMs.assign(frequencies.size(), missing);
  result.excessMs.assign(frequencies.size(), missing);
  const auto scale = 1000.0 / sampleRate;
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    if (!totalValid[index] || !minimumValid[index]) continue;
    result.valid[index] = 1;
    result.totalMs[index] = totalGrid[index] * scale;
    result.minimumMs[index] = minimumGrid[index] * scale;
    result.excessMs[index] = result.totalMs[index] - result.minimumMs[index];
  }
  return result;
}

GroupDelay combineGroupDelay(const GroupDelay &first, const GroupDelay &second) {
  if (first.valid.size() != second.valid.size()) throw std::invalid_argument("Room EQ delay grids differ");
  auto output = GroupDelay{};
  const auto count = first.valid.size();
  output.valid.resize(count); output.totalMs.assign(count, missing);
  output.minimumMs.assign(count, missing); output.excessMs.assign(count, missing);
  for (auto index = std::size_t{0}; index < count; ++index) {
    if (!first.valid[index] || !second.valid[index]) continue;
    output.valid[index] = 1;
    output.totalMs[index] = first.totalMs[index] + second.totalMs[index];
    output.minimumMs[index] = first.minimumMs[index] + second.minimumMs[index];
    output.excessMs[index] = output.totalMs[index] - output.minimumMs[index];
  }
  return output;
}

GroupDelay averageGroupDelay(std::span<const GroupDelay> analyses) {
  if (analyses.empty()) throw std::invalid_argument("Room EQ delay average requires an observation");
  auto output = GroupDelay{};
  const auto size = analyses.front().valid.size();
  for (const auto &analysis : analyses)
    if (analysis.valid.size() != size) throw std::invalid_argument("Room EQ delay grids differ");
  output.valid.resize(size); output.totalMs.assign(size, missing);
  output.minimumMs.assign(size, missing); output.excessMs.assign(size, missing);
  for (auto index = std::size_t{0}; index < size; ++index) {
    auto total = 0.0, minimum = 0.0;
    auto count = std::size_t{0};
    for (const auto &analysis : analyses) {
      if (!analysis.valid[index]) continue;
      total += analysis.totalMs[index]; minimum += analysis.minimumMs[index]; ++count;
    }
    if (!count) continue;
    output.valid[index] = 1;
    output.totalMs[index] = total / count; output.minimumMs[index] = minimum / count;
    output.excessMs[index] = output.totalMs[index] - output.minimumMs[index];
  }
  return output;
}

static std::vector<double> smoothRuns(std::span<const double> frequencies,
    std::span<const double> values, std::span<const std::uint8_t> valid, double smoothing) {
  auto output = std::vector<double>(values.begin(), values.end());
  auto first = std::size_t{0};
  while (first < output.size()) {
    while (first < output.size() && !(valid[first] && std::isfinite(output[first]))) output[first++] = missing;
    auto last = first;
    while (last < output.size() && valid[last] && std::isfinite(output[last])) ++last;
    if (last > first) {
      const auto smoothed = smooth(frequencies.subspan(first, last - first), values.subspan(first, last - first), smoothing);
      std::copy(smoothed.begin(), smoothed.end(), output.begin() + first);
    }
    first = last;
  }
  return output;
}

DisplayGroupDelay smoothGroupDelay(const GroupDelay &analysis,
    std::span<const double> frequencies, double smoothing) {
  const auto total = smoothRuns(frequencies, analysis.totalMs, analysis.valid, smoothing);
  const auto minimum = smoothRuns(frequencies, analysis.minimumMs, analysis.valid, smoothing);
  auto output = DisplayGroupDelay{analysis.valid, {}, {}, {}};
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    output.total.push_back(static_cast<float>(total[index]));
    output.minimum.push_back(static_cast<float>(minimum[index]));
    output.excess.push_back(static_cast<float>(analysis.valid[index] ? total[index] - minimum[index] : missing));
  }
  return output;
}

std::vector<double> integrateGroupDelayPhase(std::span<const double> frequencies,
    std::span<const double> delaysSeconds, std::span<const std::uint8_t> valid,
    std::span<const double> wrappedPhase) {
  auto phase = std::vector<double>(wrappedPhase.begin(), wrappedPhase.end());
  auto first = std::size_t{0};
  while (first < phase.size()) {
    while (first < phase.size() && !valid[first]) ++first;
    if (first >= phase.size()) break;
    auto last = first;
    while (last + 1 < phase.size() && valid[last + 1]) ++last;
    auto anchor = first;
    for (auto index = first + 1; index <= last; ++index)
      if (std::abs(frequencies[index] - 1000) < std::abs(frequencies[anchor] - 1000)) anchor = index;
    for (auto index = anchor + 1; index <= last; ++index) {
      const auto delta = 2 * std::numbers::pi * (frequencies[index] - frequencies[index - 1]);
      phase[index] = phase[index - 1] - 0.5 * (delaysSeconds[index - 1] + delaysSeconds[index]) * delta;
    }
    for (auto index = anchor; index-- > first;) {
      const auto delta = 2 * std::numbers::pi * (frequencies[index + 1] - frequencies[index]);
      phase[index] = phase[index + 1] + 0.5 * (delaysSeconds[index] + delaysSeconds[index + 1]) * delta;
    }
    first = last + 1;
  }
  return phase;
}

} // namespace pipetune::assets::room_eq
