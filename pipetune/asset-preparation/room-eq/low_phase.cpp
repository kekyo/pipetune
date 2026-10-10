/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "low_phase.h"
#include "group_delay.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace pipetune::assets::room_eq {

struct LowObservation {
  std::size_t first = 0, anchor = 0;
  std::vector<double> intervals, hybrid, blend, reliability, window;
  bool coverageLimited = false;
};

static GroupDelay windowAnalysis(const DirectSpectrum &direct, std::span<const double> frequencies,
    const RoomEqConfiguration &config, double windowSeconds, double taperSeconds) {
  const auto onset = direct.onsetIndex;
  const auto preroll = static_cast<std::size_t>(std::round(config.sampleRate * 0.001));
  const auto start = onset > preroll ? onset - preroll : 0;
  const auto requested = static_cast<std::size_t>(std::max(1.0, std::floor(windowSeconds * config.sampleRate)));
  const auto end = std::min(direct.samples.size(), onset + requested);
  const auto fade = std::max(1.0, std::min(static_cast<double>(end - onset), std::floor(taperSeconds * config.sampleRate)));
  const auto fadeStart = end - fade;
  auto input = std::vector<double>(end - start);
  for (auto index = start; index < end; ++index) {
    const auto gain = index < fadeStart ? 1 : 0.5 + 0.5 * std::cos(std::numbers::pi * (index - fadeStart) / fade);
    input[index - start] = direct.samples[index] * gain;
  }
  return analyzeGroupDelay(input, onset - start, config.sampleRate, frequencies, 0);
}

static double windowMagnitude(const GroupDelay &analysis, double frequency, std::uint32_t rate) {
  const auto position = frequency * analysis.fftSize / rate;
  const auto last = analysis.spectrum.real.size() - 1;
  const auto lower = std::min(last, static_cast<std::size_t>(std::floor(position))), upper = std::min(last, lower + 1);
  const auto fraction = lower == upper ? 0 : position - lower;
  const auto real = analysis.spectrum.real[lower] + fraction * (analysis.spectrum.real[upper] - analysis.spectrum.real[lower]);
  const auto imag = analysis.spectrum.imag[lower] + fraction * (analysis.spectrum.imag[upper] - analysis.spectrum.imag[lower]);
  return std::hypot(real, imag);
}

static std::optional<LowObservation> lowObservation(const DirectSpectrum &direct,
    std::span<const double> frequencies, const RoomEqConfiguration &config) {
  const auto low = config.lowFrequency;
  const auto high = std::min({phaseLowFrequency(config), config.highFrequency, config.sampleRate * 0.45});
  if (low >= high || direct.samples.empty()) return std::nullopt;
  auto result = LowObservation{};
  while (result.first < frequencies.size() && frequencies[result.first] < low) ++result.first;
  result.anchor = result.first;
  while (result.anchor < frequencies.size() && frequencies[result.anchor] < high) ++result.anchor;
  if (result.anchor >= frequencies.size() || result.anchor <= result.first) return std::nullopt;
  auto magnitude = direct.magnitude, excess = direct.excessDelaySeconds;
  auto valid = direct.valid;
  auto coverage = std::vector<double>(frequencies.size());
  result.window.resize(frequencies.size());
  const auto available = (direct.samples.size() - direct.onsetIndex) / static_cast<double>(config.sampleRate);
  const auto window = config.directWindowMs / 1000;
  const auto taper = std::min(config.directWindowMs, 1000 / phaseLowFrequency(config)) / 1000;
  const auto octaves = static_cast<std::size_t>(std::ceil(std::log2(high / low)));
  auto analyses = std::vector<GroupDelay>{};
  for (auto octave = std::size_t{0}; octave <= octaves; ++octave) {
    const auto factor = std::pow(2, octave);
    analyses.push_back(windowAnalysis(direct, frequencies, config, window * factor, taper * factor));
  }
  for (auto index = result.first; index <= result.anchor; ++index) {
    const auto frequency = frequencies[index], requested = window * high / frequency;
    result.window[index] = std::min(requested, available);
    const auto position = std::max(0.0, std::log2(high / frequency));
    const auto lower = std::min(octaves, static_cast<std::size_t>(std::floor(position))), upper = std::min(octaves, lower + 1);
    const auto fraction = upper == lower ? 0 : position - lower;
    if (!analyses[lower].valid[index] || !analyses[upper].valid[index]) {
      excess[index] = std::numeric_limits<double>::quiet_NaN(); valid[index] = 0;
    } else {
      excess[index] = (analyses[lower].excessMs[index] * (1 - fraction) + analyses[upper].excessMs[index] * fraction) / 1000;
      magnitude[index] = windowMagnitude(analyses[lower], frequency, config.sampleRate) * (1 - fraction) +
                         windowMagnitude(analyses[upper], frequency, config.sampleRate) * fraction;
      valid[index] = 1;
    }
    coverage[index] = requested > available ? available / requested : 1;
    result.coverageLimited = result.coverageLimited || requested > available;
  }
  const auto fixed = directPhaseAnalysis(direct, frequencies, config, false);
  const auto smoothed = smoothSynthesisRuns(excess, valid, frequencies, config.phaseSmoothing.value_or(config.smoothing));
  result.intervals.resize(result.anchor + 1); result.hybrid.resize(result.anchor + 1);
  result.blend.resize(result.anchor + 1); result.reliability.resize(result.anchor + 1);
  const auto blendStart = high / std::pow(2, 1.0 / 3);
  for (auto index = result.first + 1; index <= result.anchor; ++index) {
    const auto validDelay = valid[index] && std::isfinite(smoothed[index]);
    const auto hybrid = validDelay ? std::max(0.0, smoothed[index]) : 0;
    const auto midpoint = std::sqrt(frequencies[index - 1] * frequencies[index]);
    auto blend = 0.0;
    if (midpoint > blendStart) blend = 0.5 - 0.5 * std::cos(std::numbers::pi * std::min(1.0, std::log2(midpoint / blendStart) * 3));
    result.hybrid[index] = hybrid; result.blend[index] = blend;
    result.intervals[index] = hybrid * (1 - blend) + fixed.intervalDelays[index] * blend;
    const auto ratio = std::min(magnitude[index - 1], magnitude[index]) / fixed.floor;
    result.reliability[index] = validDelay ? (ratio < 1 ? ratio * ratio : 1) * std::min(coverage[index - 1], coverage[index]) : 0;
  }
  return result;
}

double edgeEnergyRatio(std::span<const float> taps) {
  const auto edge = static_cast<std::size_t>(std::max(1.0, std::ceil(taps.size() * 0.05)));
  auto total = 0.0, edges = 0.0;
  for (auto index = std::size_t{0}; index < taps.size(); ++index) {
    const auto energy = static_cast<double>(taps[index]) * taps[index];
    total += energy;
    if (index < edge || index >= taps.size() - edge) edges += energy;
  }
  return edges / (total > 0 ? total : 1);
}

bool phaseEnergySafe(const RenderedFilter &candidate, const RenderedFilter &baseline) {
  return edgeEnergyRatio(candidate.taps) <= std::max(0.002, edgeEnergyRatio(baseline.taps) * 1.05 + 1e-8);
}

static double residualRms(const RenderedFilter &filter, const GroupDelay &reference,
    std::span<const double> frequencies, double low, double high, const RoomEqConfiguration &config) {
  const auto samples = std::vector<double>(filter.taps.begin(), filter.taps.end());
  const auto delay = analyzeGroupDelay(samples, config.taps / 2, config.sampleRate, frequencies, config.taps * 2);
  const auto display = smoothGroupDelay(combineGroupDelay(reference, delay), frequencies, config.smoothing);
  auto squared = 0.0, weightSum = 0.0;
  for (auto index = std::size_t{1}; index < frequencies.size(); ++index) {
    if (frequencies[index] < low || frequencies[index] > high || !display.valid[index] || !std::isfinite(display.excess[index])) continue;
    const auto weight = std::log(frequencies[index] / frequencies[index - 1]);
    const auto magnitude = std::abs(static_cast<double>(display.excess[index])) / 1000;
    squared += magnitude * magnitude * weight; weightSum += weight;
  }
  return weightSum == 0 ? 0 : std::sqrt(squared / weightSum);
}

static RenderedFilter renderLowCandidate(std::span<const double> magnitudes, std::span<const double> phase,
    std::span<const double> correction, double amount, std::span<const double> frequencies,
    std::size_t first, double alignment, const RoomEqConfiguration &config) {
  auto candidateMagnitudes = std::vector<double>(magnitudes.begin(), magnitudes.end());
  auto candidatePhase = std::vector<double>(phase.begin(), phase.end());
  const auto endpoint = correction[first] * amount, real = std::cos(endpoint), imag = -std::sin(endpoint);
  for (auto bin = std::size_t{1}; bin < phase.size(); ++bin) {
    if (bin >= first) candidatePhase[bin] -= correction[bin] * amount;
    else if (frequencies[bin] >= 20) candidatePhase[bin] -= endpoint;
    else {
      // Close the phase through a complex chord below 20 Hz, preserving DC.
      const auto weight = frequencies[bin] <= 0 ? 0 : 0.5 - 0.5 * std::cos(std::numbers::pi * frequencies[bin] / 20);
      const auto r = 1 - weight + weight * real, i = weight * imag;
      candidateMagnitudes[bin] *= std::hypot(r, i); candidatePhase[bin] += std::atan2(i, r);
    }
  }
  for (auto bin = std::size_t{0}; bin < phase.size(); ++bin)
    candidatePhase[bin] += 2 * std::numbers::pi * frequencies[bin] * alignment -
                          2 * std::numbers::pi * bin / (config.taps * 2) * (config.taps / 2);
  return renderSynthesis(candidateMagnitudes, candidatePhase, config);
}

LowPhaseDesign extendLowPhase(const DirectSpectrum &target, std::span<const DirectSpectrum> directs,
    std::span<const double> upperCorrection, double alignment, std::span<const double> magnitudes,
    std::span<const double> unalignedPhase, std::span<const double> frequencies,
    std::span<const double> diagnosticFrequencies, const RoomEqConfiguration &config,
    RenderedFilter baseline, const std::optional<ReverbTarget> &reverb) {
  auto diagnostic = RoomEqDiagnostic{};
  diagnostic.state = "disabled"; diagnostic.scale = 0;
  diagnostic.reason = !config.lowFrequencyPhaseExtension ? "notRequested" :
      config.phaseCorrectionAmount == 0 ? "phaseCorrectionDisabled" : "insufficientData";
  if (!config.lowFrequencyPhaseExtension || config.phaseCorrectionAmount == 0) return {std::move(baseline), diagnostic};
  const auto observation = lowObservation(target, frequencies, config);
  if (!observation) return {std::move(baseline), diagnostic};
  const auto &low = *observation;
  auto points = std::vector<std::optional<LowObservation>>{};
  for (const auto &direct : directs) points.push_back(lowObservation(direct, frequencies, config));
  auto coverageLimited = low.coverageLimited;
  for (const auto &point : points) coverageLimited = coverageLimited || (point && point->coverageLimited);
  auto upperDelay = low.intervals[low.anchor];
  if (low.anchor + 1 < frequencies.size()) {
    const auto omega = 2 * std::numbers::pi * (frequencies[low.anchor + 1] - frequencies[low.anchor]);
    if (omega > 0) upperDelay = -(upperCorrection[low.anchor + 1] - upperCorrection[low.anchor]) / omega;
  }
  auto common = std::vector<double>(frequencies.size());
  for (auto index = low.first + 1; index <= low.anchor; ++index) {
    common[index] = low.hybrid[index];
    if (points.size() <= 1) continue;
    auto weighted = 0.0, weightSum = 0.0;
    for (const auto &point : points) {
      if (!point || point->reliability[index] <= 0) continue;
      weighted += point->hybrid[index] * point->reliability[index]; weightSum += point->reliability[index];
    }
    if (weightSum > 0) common[index] = weighted / weightSum;
  }
  auto correction = std::vector<double>(frequencies.size());
  for (auto index = low.anchor; index > low.first; --index) {
    auto delay = (common[index] - alignment / config.phaseCorrectionAmount) * (1 - low.blend[index]) + upperDelay * low.blend[index];
    if (reverb) {
      if (reverb->windowSeconds >= low.window[index]) delay *= 1 - reverb->amountPerPhaseAmount;
      else delay -= reverb->delays[index];
    }
    correction[index - 1] = correction[index] + delay * 2 * std::numbers::pi * (frequencies[index] - frequencies[index - 1]);
  }
  auto activeLast = low.first;
  for (auto index = low.first + 1; index <= low.anchor && low.blend[index] == 0; ++index) activeLast = index;
  const auto activeLow = frequencies[low.first], activeHigh = frequencies[activeLast];
  const auto samples = std::vector<double>(target.groupDelaySamples.begin(), target.groupDelaySamples.end());
  const auto reference = analyzeGroupDelay(samples, target.groupDelayOnsetIndex, config.sampleRate, diagnosticFrequencies, 0);
  const auto baselineRms = residualRms(baseline, reference, diagnosticFrequencies, activeLow, activeHigh, config);
  const auto tolerance = std::max(0.0001, baselineRms * 0.02);
  auto reason = std::optional<std::string>{};
  const auto evaluate = [&](double scale) {
    auto candidate = renderLowCandidate(magnitudes, unalignedPhase, correction,
        config.phaseCorrectionAmount * scale, frequencies, low.first, alignment, config);
    const auto energySafe = phaseEnergySafe(candidate, baseline);
    const auto delaySafe = residualRms(candidate, reference, diagnosticFrequencies, activeLow, activeHigh, config) <= baselineRms + tolerance;
    if (!reason) {
      if (!delaySafe) reason = "groupDelay";
      else if (!energySafe) reason = "firEnergy";
    }
    return std::pair{std::move(candidate), energySafe && delaySafe};
  };
  auto safeScale = 0.0, unsafeScale = 1.0;
  auto safe = RenderedFilter{};
  for (auto step = 0; step < 16; ++step) {
    const auto scale = 1 - step / 16.0;
    auto [candidate, accepted] = evaluate(scale);
    if (accepted) { safeScale = scale; safe = std::move(candidate); break; }
    unsafeScale = scale;
  }
  if (safeScale == 1) {
    diagnostic.state = coverageLimited ? "reduced" : "applied"; diagnostic.scale = 1;
    diagnostic.reason = coverageLimited ? std::optional<std::string>("insufficientData") : std::nullopt;
    return {std::move(safe), std::move(diagnostic)};
  }
  for (auto step = 0; step < 8; ++step) {
    const auto scale = (safeScale + unsafeScale) / 2;
    auto [candidate, accepted] = evaluate(scale);
    if (accepted) { safeScale = scale; safe = std::move(candidate); }
    else unsafeScale = scale;
  }
  diagnostic.state = safeScale > 0 ? "reduced" : "disabled";
  diagnostic.scale = safeScale; diagnostic.reason = reason.value_or("groupDelay");
  return {safeScale > 0 ? std::move(safe) : std::move(baseline), std::move(diagnostic)};
}

} // namespace pipetune::assets::room_eq
