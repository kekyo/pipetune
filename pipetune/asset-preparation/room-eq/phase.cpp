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
#include "low_phase.h"
#include "reverb.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace pipetune::assets::room_eq {

static void diagnosePhaseWindow(RoomEqDiagnostic &diagnostic, std::span<const ImpulseAnalysis> analyses,
    const RenderedFilter &filter, std::span<const double> frequencies, const RoomEqConfiguration &config) {
  if (diagnostic.state != "applied") return;
  auto sourceDelays = std::vector<GroupDelay>{};
  for (const auto &analysis : analyses) {
    const auto samples = std::vector<double>(analysis.samples.begin(), analysis.samples.end());
    sourceDelays.push_back(analyzeGroupDelay(samples, analysis.onsetIndex, config.sampleRate, frequencies, 0));
  }
  const auto beforeDelay = averageGroupDelay(sourceDelays);
  const auto samples = std::vector<double>(filter.taps.begin(), filter.taps.end());
  const auto filterDelay = analyzeGroupDelay(samples, config.taps / 2, config.sampleRate, frequencies, config.taps * 2);
  const auto before = smoothGroupDelay(beforeDelay, frequencies, config.smoothing);
  const auto after = smoothGroupDelay(combineGroupDelay(beforeDelay, filterDelay), frequencies, config.smoothing);
  auto reference = std::optional<std::size_t>{};
  auto distance = std::numeric_limits<double>::infinity();
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    if (!std::isfinite(before.excess[index]) || !std::isfinite(after.excess[index])) continue;
    const auto candidate = std::abs(std::log(frequencies[index] / 1000));
    if (candidate < distance) { reference = index; distance = candidate; }
  }
  const auto beforeReference = reference ? before.excess[*reference] : 0;
  const auto afterReference = reference ? after.excess[*reference] : 0;
  auto beforeMaximum = 0.0, afterMaximum = 0.0;
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    const auto b = std::abs(static_cast<double>(before.excess[index]) - beforeReference);
    const auto a = std::abs(static_cast<double>(after.excess[index]) - afterReference);
    if (std::isfinite(b)) beforeMaximum = std::max(beforeMaximum, b);
    if (std::isfinite(a)) afterMaximum = std::max(afterMaximum, a);
  }
  const auto available = (config.taps / 2.0 - 1) / config.sampleRate * 1000;
  if (beforeMaximum > available && afterMaximum > available) {
    diagnostic.state = "reduced"; diagnostic.scale = available / beforeMaximum;
    diagnostic.reason = "firWindow"; diagnostic.residualMaximumMs = afterMaximum;
  }
}

PhaseDesign designPhase(std::span<const ImpulseAnalysis> analyses, std::span<const RoomEqImpulse> impulses,
    std::span<const double> magnitudes, std::span<const double> frequencies,
    std::span<const double> diagnosticFrequencies, const RoomEqConfiguration &config) {
  if (analyses.empty()) throw std::invalid_argument("Room EQ Correction requires complete impulse responses");
  auto selected = analyses;
  if (config.referencePoint > 0)
    for (auto index = std::size_t{0}; index < impulses.size(); ++index)
      if (impulses[index].pointId + 1 == config.referencePoint) { selected = analyses.subspan(index, 1); break; }
  const auto reference = alignedAverageAnalysis(selected, config);
  const auto taper = std::min(config.directWindowMs, 1000 / phaseLowFrequency(config));
  const auto timing = directSpectrum(reference, config, frequencies, config.directWindowMs, taper);
  auto directs = std::vector<DirectSpectrum>{};
  auto effectiveWindow = std::max(config.directWindowMs, config.reverbWindowMs);
  for (const auto &analysis : selected) {
    directs.push_back(directSpectrum(analysis, config, frequencies, config.directWindowMs, taper));
    effectiveWindow = std::min(effectiveWindow, (analysis.samples.size() - analysis.onsetIndex) * 1000.0 / config.sampleRate);
  }
  const auto correction = consensusDirectPhase(directs, frequencies, config);
  const auto minimum = minimumPhase(magnitudes);
  auto phase = minimum;
  for (auto bin = std::size_t{0}; bin < phase.size(); ++bin) phase[bin] -= correction[bin] * config.phaseCorrectionAmount;
  const auto alignment = config.phaseCorrectionAmount > 0
      ? timingAlignment(timing, magnitudes, minimum, phase, config, config.directWindowMs) : 0;
  auto unaligned = phase;
  for (auto bin = std::size_t{0}; bin < phase.size(); ++bin)
    phase[bin] += 2 * std::numbers::pi * frequencies[bin] * alignment -
                  2 * std::numbers::pi * bin / (config.taps * 2) * (config.taps / 2);
  auto result = PhaseDesign{};
  auto reverb = designReverb(selected, timing, correction, minimum, magnitudes, frequencies, config,
      renderSynthesis(magnitudes, phase, config), std::move(unaligned), alignment, effectiveWindow);
  auto extended = extendLowPhase(timing, directs, correction, reverb.alignment, magnitudes, reverb.phase,
      frequencies, diagnosticFrequencies, config, std::move(reverb.filter), reverb.target);
  result.filter = std::move(extended.filter);
  result.phase.state = config.phaseCorrectionAmount > 0 ? "applied" : "notRequested";
  result.phase.scale = config.phaseCorrectionAmount > 0 ? 1 : 0;
  if (config.phaseCorrectionAmount == 0) result.phase.reason = "phaseCorrectionDisabled";
  result.low = std::move(extended.diagnostic);
  result.reverb = std::move(reverb.diagnostic);
  diagnosePhaseWindow(result.phase, selected, result.filter, diagnosticFrequencies, config);
  return result;
}

} // namespace pipetune::assets::room_eq
