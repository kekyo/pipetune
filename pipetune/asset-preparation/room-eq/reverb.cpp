/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "reverb.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace pipetune::assets::room_eq {

struct ReverbConsensus {
  std::vector<double> intervals;
  double agreementMinimum = 1;
  std::size_t first = 0;
  double low = 0, high = 0;
};

static ReverbConsensus extendedConsensus(std::span<const ImpulseAnalysis> observations,
    std::span<const double> frequencies, const RoomEqConfiguration &config, double effectiveWindow) {
  auto extended = config;
  extended.directWindowMs = effectiveWindow;
  extended.smoothing = config.reverbSmoothing; extended.phaseSmoothing = config.reverbSmoothing;
  extended.highFrequency = std::min(config.reverbMaxFrequency, config.highFrequency);
  extended.phaseLowFrequency.reset();
  const auto taper = std::min(effectiveWindow, 1000 / std::max(config.lowFrequency, 3000 / effectiveWindow));
  auto directs = std::vector<DirectSpectrum>{};
  auto analyses = std::vector<DirectPhaseAnalysis>{};
  for (const auto &observation : observations) {
    directs.push_back(directSpectrum(observation, extended, frequencies, effectiveWindow, taper));
    analyses.push_back(directPhaseAnalysis(directs.back(), frequencies, extended, true));
  }
  auto result = ReverbConsensus{};
  result.first = analyses.front().first; result.low = analyses.front().low; result.high = analyses.front().high;
  if (analyses.size() == 1) { result.intervals = std::move(analyses.front().intervalDelays); return result; }
  result.intervals.resize(frequencies.size());
  const auto upper = std::min(result.high * std::pow(2, 1.0 / 3), config.sampleRate * 0.48);
  const auto smoothingScale = 2 * std::numbers::pi * (std::pow(2, config.reverbSmoothing) - 1);
  for (auto index = std::max<std::size_t>(1, result.first); index < frequencies.size() && frequencies[index] <= upper; ++index) {
    auto delaySum = 0.0, real = 0.0, imag = 0.0, weightSum = 0.0;
    for (auto point = std::size_t{0}; point < analyses.size(); ++point) {
      const auto delay = analyses[point].intervalDelays[index];
      if (!analyses[point].valid[index] || !std::isfinite(delay)) continue;
      const auto reliability = directs[point].magnitude[index] / analyses[point].floor;
      const auto weight = reliability < 1 ? reliability * reliability : 1;
      if (weight <= 0) continue;
      delaySum += delay * weight;
      const auto theta = delay * smoothingScale * frequencies[index];
      real += std::cos(theta) * weight; imag += std::sin(theta) * weight; weightSum += weight;
    }
    if (weightSum > 0) {
      result.intervals[index] = delaySum / weightSum;
      const auto agreement = std::hypot(real, imag) / weightSum;
      if (index > result.first && frequencies[index] <= result.high)
        result.agreementMinimum = std::min(result.agreementMinimum, agreement);
    }
  }
  return result;
}

ReverbDesign designReverb(std::span<const ImpulseAnalysis> analyses, const DirectSpectrum &timing,
    std::span<const double> directCorrection, std::span<const double> minimum,
    std::span<const double> magnitudes, std::span<const double> frequencies,
    const RoomEqConfiguration &config, RenderedFilter baseline, std::vector<double> unalignedPhase,
    double alignment, double effectiveWindow) {
  auto result = ReverbDesign{};
  result.filter = std::move(baseline); result.phase = std::move(unalignedPhase); result.alignment = alignment;
  result.diagnostic.state = "notRequested"; result.diagnostic.effectiveWindowMs = effectiveWindow;
  if (config.reverbAmount == 0) return result;
  result.diagnostic.state = "disabled";
  if (effectiveWindow <= config.directWindowMs) { result.diagnostic.reason = "windowBudget"; return result; }
  if (std::max(config.lowFrequency, 3000 / effectiveWindow) >=
      std::min({config.reverbMaxFrequency, config.highFrequency, config.sampleRate * 0.45})) {
    result.diagnostic.reason = "emptyBand"; return result;
  }
  const auto consensus = extendedConsensus(analyses, frequencies, config, effectiveWindow);
  result.diagnostic.agreementMinimum = consensus.agreementMinimum;
  const auto deltaOmega = 2 * std::numbers::pi * (frequencies[1] - frequencies[0]);
  auto reverbPhase = std::vector<double>(frequencies.size());
  for (auto bin = consensus.first + 1; bin < frequencies.size(); ++bin) {
    const auto weight = correctionWeight(frequencies[bin], consensus.low, consensus.high, config.sampleRate * 0.48);
    auto delay = 0.0;
    if (weight > 0) {
      const auto directDelay = -(directCorrection[bin] - directCorrection[bin - 1]) / deltaOmega;
      delay = weight * (consensus.intervals[bin] - directDelay);
    }
    reverbPhase[bin] = reverbPhase[bin - 1] - delay * deltaOmega;
  }
  const auto buildPhase = [&](double scale) {
    auto phase = std::vector<double>(minimum.begin(), minimum.end());
    for (auto bin = std::size_t{0}; bin < phase.size(); ++bin)
      phase[bin] -= directCorrection[bin] * config.phaseCorrectionAmount + reverbPhase[bin] * config.reverbAmount * scale;
    return phase;
  };
  const auto fullPhase = buildPhase(1);
  // All candidates share the full-scale timing alignment, while the guard
  // baseline retains its own direct-only alignment and compact impulse.
  const auto candidateAlignment = timingAlignment(timing, magnitudes, minimum, fullPhase,
      config, std::max(config.directWindowMs, effectiveWindow));
  for (const auto scale : {1.0, 0.5, 0.25}) {
    auto phase = scale == 1 ? fullPhase : buildPhase(scale);
    auto aligned = phase;
    for (auto bin = std::size_t{0}; bin < phase.size(); ++bin)
      aligned[bin] += 2 * std::numbers::pi * frequencies[bin] * candidateAlignment -
                      2 * std::numbers::pi * bin / (config.taps * 2) * (config.taps / 2);
    auto candidate = renderSynthesis(magnitudes, aligned, config);
    if (!phaseEnergySafe(candidate, result.filter)) continue;
    result.filter = std::move(candidate); result.phase = std::move(phase); result.alignment = candidateAlignment;
    result.diagnostic.state = scale == 1 ? "applied" : "reduced"; result.diagnostic.scale = scale;
    if (scale != 1) result.diagnostic.reason = "firEnergy";
    if (config.phaseCorrectionAmount > 0) {
      auto target = ReverbTarget{};
      target.amountPerPhaseAmount = config.reverbAmount * scale / config.phaseCorrectionAmount;
      target.windowSeconds = effectiveWindow / 1000;
      target.delays.resize(frequencies.size());
      for (auto bin = std::size_t{1}; bin < frequencies.size(); ++bin)
        target.delays[bin] = -(reverbPhase[bin] - reverbPhase[bin - 1]) / deltaOmega * target.amountPerPhaseAmount;
      result.target = std::move(target);
    }
    return result;
  }
  result.diagnostic.scale = 0; result.diagnostic.reason = "firEnergy";
  return result;
}

} // namespace pipetune::assets::room_eq
