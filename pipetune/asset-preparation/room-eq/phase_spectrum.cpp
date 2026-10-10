/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "phase.h"
#include "group_delay.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>

namespace pipetune::assets::room_eq {

double phaseLowFrequency(const RoomEqConfiguration &config) {
  return config.phaseLowFrequency ? std::max(1000 / config.directWindowMs, *config.phaseLowFrequency)
                                 : std::max(config.lowFrequency, 3000 / config.directWindowMs);
}

double correctionWeight(double frequency, double low, double high, double upperLimit) {
  constexpr auto flank = 1.0 / 3;
  if (frequency >= low && frequency <= high) return 1;
  if (frequency < low) {
    const auto edge = low / std::pow(2, flank);
    return frequency <= edge ? 0 : 0.5 - 0.5 * std::cos(std::numbers::pi * std::log2(frequency / edge) / flank);
  }
  const auto edge = std::min(high * std::pow(2, flank), upperLimit);
  return frequency >= edge ? 0 : 0.5 - 0.5 * std::cos(std::numbers::pi * std::log2(edge / frequency) / flank);
}

ImpulseAnalysis alignedAverageAnalysis(std::span<const ImpulseAnalysis> analyses, const RoomEqConfiguration &config) {
  if (analyses.size() == 1) return analyses.front();
  auto result = ImpulseAnalysis{};
  result.onsetIndex = std::max(1.0, std::round(config.sampleRate * 0.005));
  const auto duration = std::max(2.0, std::round(config.sampleRate * std::max(5.0, config.directWindowMs) / 1000));
  result.samples.resize(result.onsetIndex + config.taps / 2 + static_cast<std::size_t>(duration));
  for (auto index = std::size_t{0}; index < result.samples.size(); ++index) {
    const auto relative = static_cast<std::int64_t>(index) - static_cast<std::int64_t>(result.onsetIndex);
    auto sum = 0.0;
    auto count = std::size_t{0};
    for (const auto &analysis : analyses) {
      const auto source = static_cast<std::int64_t>(analysis.onsetIndex) + relative;
      if (source < 0 || static_cast<std::size_t>(source) >= analysis.samples.size()) continue;
      sum += analysis.samples[source]; ++count;
    }
    if (count) result.samples[index] = static_cast<float>(sum / count);
  }
  result.fftSize = std::bit_ceil(result.samples.size());
  auto postLength = std::size_t{1};
  for (const auto &analysis : analyses) {
    result.groupDelayOnsetIndex = std::max(result.groupDelayOnsetIndex, analysis.onsetIndex);
    postLength = std::max(postLength, analysis.samples.size() - analysis.onsetIndex);
  }
  result.groupDelaySamples.resize(result.groupDelayOnsetIndex + postLength);
  for (auto index = std::size_t{0}; index < result.groupDelaySamples.size(); ++index) {
    const auto relative = static_cast<std::int64_t>(index) - static_cast<std::int64_t>(result.groupDelayOnsetIndex);
    auto sum = 0.0;
    auto count = std::size_t{0};
    for (const auto &analysis : analyses) {
      const auto source = static_cast<std::int64_t>(analysis.onsetIndex) + relative;
      if (source < 0 || static_cast<std::size_t>(source) >= analysis.samples.size()) continue;
      sum += analysis.samples[source]; ++count;
    }
    if (count) result.groupDelaySamples[index] = static_cast<float>(sum / count);
  }
  return result;
}

DirectSpectrum directSpectrum(const ImpulseAnalysis &analysis, const RoomEqConfiguration &config,
    std::span<const double> frequencies, double windowMs, double taperMs) {
  auto input = std::vector<double>(analysis.fftSize);
  const auto start = std::max<std::int64_t>(0, static_cast<std::int64_t>(analysis.onsetIndex) - static_cast<std::int64_t>(std::round(config.sampleRate * 0.001)));
  const auto end = std::min(analysis.samples.size(), analysis.onsetIndex +
      static_cast<std::size_t>(std::max(1.0, std::round(config.sampleRate * windowMs / 1000))));
  const auto fadeLength = std::max(1.0, std::min(static_cast<double>(end - analysis.onsetIndex), std::round(config.sampleRate * taperMs / 1000)));
  const auto fadeStart = end - fadeLength;
  for (auto index = static_cast<std::size_t>(start); index < end; ++index) {
    const auto gain = index >= fadeStart ? 0.5 + 0.5 * std::cos(std::numbers::pi * (index - fadeStart) / fadeLength) : 1;
    input[index] = analysis.samples[index] * gain;
  }
  const auto spectrum = realTransform(input);
  auto sourceFrequencies = std::vector<double>(spectrum.real.size() - 1), magnitudes = sourceFrequencies;
  for (auto bin = std::size_t{1}; bin < spectrum.real.size(); ++bin) {
    sourceFrequencies[bin - 1] = static_cast<double>(bin) * config.sampleRate / analysis.fftSize;
    magnitudes[bin - 1] = std::hypot(spectrum.real[bin], spectrum.imag[bin]);
  }
  auto delay = analyzeGroupDelay(input, analysis.onsetIndex, config.sampleRate, frequencies, analysis.fftSize);
  auto wrapped = std::vector<double>(frequencies.size());
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    const auto position = frequencies[index] * analysis.fftSize / config.sampleRate;
    const auto lower = std::min(spectrum.real.size() - 1, static_cast<std::size_t>(std::floor(position)));
    const auto upper = std::min(spectrum.real.size() - 1, lower + 1);
    const auto fraction = upper == lower ? 0 : position - lower;
    const auto real = spectrum.real[lower] + fraction * (spectrum.real[upper] - spectrum.real[lower]);
    const auto imag = spectrum.imag[lower] + fraction * (spectrum.imag[upper] - spectrum.imag[lower]);
    wrapped[index] = std::atan2(imag, real) + 2 * std::numbers::pi * frequencies[index] * analysis.onsetIndex / config.sampleRate;
    delay.totalMs[index] /= 1000; delay.excessMs[index] /= 1000;
  }
  auto result = DirectSpectrum{};
  result.magnitude = interpolate(sourceFrequencies, magnitudes, frequencies);
  result.phase = integrateGroupDelayPhase(frequencies, delay.totalMs, delay.valid, wrapped);
  result.excessDelaySeconds = std::move(delay.excessMs);
  result.valid = std::move(delay.valid);
  result.samples = analysis.samples; result.onsetIndex = analysis.onsetIndex;
  result.groupDelaySamples = analysis.groupDelaySamples.empty() ? std::span<const float>(analysis.samples) : std::span<const float>(analysis.groupDelaySamples);
  result.groupDelayOnsetIndex = analysis.groupDelaySamples.empty() ? analysis.onsetIndex : analysis.groupDelayOnsetIndex;
  return result;
}

std::vector<double> smoothSynthesisRuns(std::span<const double> values,
    std::span<const std::uint8_t> valid, std::span<const double> frequencies, double smoothing) {
  auto output = std::vector<double>(values.size(), std::numeric_limits<double>::quiet_NaN());
  auto first = std::size_t{0};
  while (first < values.size()) {
    while (first < values.size() && !(valid[first] && std::isfinite(values[first]))) ++first;
    auto last = first;
    while (last < values.size() && valid[last] && std::isfinite(values[last])) ++last;
    if (last > first) {
      const auto runFrequencies = frequencies.subspan(first, last - first), runValues = values.subspan(first, last - first);
      const auto low = std::max(20.0, runFrequencies.front()), high = runFrequencies.back();
      if (high > low && runFrequencies.size() > 1) {
        const auto grid = logGrid(low, high);
        const auto onGrid = interpolate(runFrequencies, runValues, grid);
        const auto smoothed = smooth(grid, onGrid, smoothing);
        const auto onRun = interpolate(grid, smoothed, runFrequencies);
        std::copy(onRun.begin(), onRun.end(), output.begin() + first);
      } else std::copy(runValues.begin(), runValues.end(), output.begin() + first);
    }
    first = last + 1;
  }
  return output;
}

DirectPhaseAnalysis directPhaseAnalysis(const DirectSpectrum &direct, std::span<const double> frequencies,
    const RoomEqConfiguration &config, bool preserveAbsoluteDelay) {
  auto result = DirectPhaseAnalysis{};
  result.low = phaseLowFrequency(config); result.high = std::min(config.highFrequency, config.sampleRate * 0.45);
  while (result.first < frequencies.size() && frequencies[result.first] < result.low) ++result.first;
  auto inBand = std::vector<double>{};
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index)
    if (frequencies[index] >= result.low && frequencies[index] <= result.high) inBand.push_back(direct.magnitude[index]);
  std::sort(inBand.begin(), inBand.end());
  result.floor = std::max(1e-8, (inBand.empty() ? 1 : inBand[inBand.size() / 2]) * 0.01);
  result.valid.resize(frequencies.size());
  auto delays = std::vector<double>(frequencies.size(), std::numeric_limits<double>::quiet_NaN());
  auto sum = 0.0, weightSum = 0.0;
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    if (!direct.valid[index] || direct.magnitude[index] <= result.floor || !std::isfinite(direct.excessDelaySeconds[index])) continue;
    result.valid[index] = 1; delays[index] = direct.excessDelaySeconds[index];
    if (frequencies[index] < result.low || frequencies[index] > result.high) continue;
    const auto weight = index == 0 ? 1 : frequencies[index] - frequencies[index - 1];
    sum += delays[index] * weight; weightSum += weight;
  }
  const auto reference = !preserveAbsoluteDelay && weightSum > 0 ? sum / weightSum : 0;
  const auto smoothed = smoothSynthesisRuns(delays, result.valid, frequencies, config.phaseSmoothing.value_or(config.smoothing));
  result.intervalDelays.resize(frequencies.size());
  for (auto index = std::size_t{1}; index < frequencies.size(); ++index)
    if (result.valid[index] && std::isfinite(smoothed[index])) result.intervalDelays[index] = smoothed[index] - reference;
  return result;
}

std::vector<double> consensusDirectPhase(std::span<const DirectSpectrum> directs,
    std::span<const double> frequencies, const RoomEqConfiguration &config) {
  auto analyses = std::vector<DirectPhaseAnalysis>{};
  for (const auto &direct : directs) analyses.push_back(directPhaseAnalysis(direct, frequencies, config, false));
  const auto &first = analyses.front();
  auto phase = std::vector<double>(frequencies.size());
  for (auto index = first.first + 1; index < frequencies.size(); ++index) {
    phase[index] = phase[index - 1];
    if (frequencies[index] > first.high) continue;
    auto delay = 0.0, weightSum = 0.0;
    for (auto point = std::size_t{0}; point < analyses.size(); ++point) {
      if (!analyses[point].valid[index] || !std::isfinite(analyses[point].intervalDelays[index])) continue;
      const auto reliability = directs[point].magnitude[index] / analyses[point].floor;
      const auto weight = reliability < 1 ? reliability * reliability : 1;
      delay += analyses[point].intervalDelays[index] * weight; weightSum += weight;
    }
    const auto interval = weightSum > 0 ? delay / weightSum : 0;
    phase[index] -= interval * 2 * std::numbers::pi * (frequencies[index] - frequencies[index - 1]);
  }
  for (auto index = std::size_t{0}; index < phase.size(); ++index)
    phase[index] *= correctionWeight(frequencies[index], first.low, first.high, config.sampleRate * 0.48);
  return phase;
}

} // namespace pipetune::assets::room_eq
