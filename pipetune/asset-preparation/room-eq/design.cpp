/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Port of EffeTune v2.13.0 js/room-eq/design-core.js and
 * js/utils/measurement-dsp/smoothing.js. Copyright (c) 2025-2026
 * Yoshiyuki Kobayashi. MIT; see LICENSE.effetune.
 */
#include "design.h"
#include "numeric.h"
#include "analysis.h"
#include "phase.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <numbers>
#include <stdexcept>

namespace pipetune::assets {

using room_eq::logGrid;
using room_eq::interpolate;
using room_eq::smooth;

static double dbToGain(double value) { return std::pow(10, value / 20); }
static double gainToDb(double value) { return 20 * std::log10(std::max(1e-8, value)); }

static double rbjMagnitude(const RoomEqBand &band, double frequency, double sampleRate) {
  const auto omega = 2 * std::numbers::pi * std::min(band.frequency, sampleRate * 0.49) / sampleRate;
  const auto cosine = std::cos(omega), sine = std::sin(omega);
  const auto amplitude = std::pow(10, band.gain / 40), alpha = sine / (2 * band.q), root = std::sqrt(amplitude);
  double b0, b1, b2, a0, a1, a2;
  if (band.type == RoomEqBandType::LowShelf) {
    b0 = amplitude * ((amplitude + 1) - (amplitude - 1) * cosine + 2 * root * alpha);
    b1 = 2 * amplitude * ((amplitude - 1) - (amplitude + 1) * cosine);
    b2 = amplitude * ((amplitude + 1) - (amplitude - 1) * cosine - 2 * root * alpha);
    a0 = (amplitude + 1) + (amplitude - 1) * cosine + 2 * root * alpha;
    a1 = -2 * ((amplitude - 1) + (amplitude + 1) * cosine);
    a2 = (amplitude + 1) + (amplitude - 1) * cosine - 2 * root * alpha;
  } else if (band.type == RoomEqBandType::HighShelf) {
    b0 = amplitude * ((amplitude + 1) + (amplitude - 1) * cosine + 2 * root * alpha);
    b1 = -2 * amplitude * ((amplitude - 1) + (amplitude + 1) * cosine);
    b2 = amplitude * ((amplitude + 1) + (amplitude - 1) * cosine - 2 * root * alpha);
    a0 = (amplitude + 1) - (amplitude - 1) * cosine + 2 * root * alpha;
    a1 = 2 * ((amplitude - 1) - (amplitude + 1) * cosine);
    a2 = (amplitude + 1) - (amplitude - 1) * cosine - 2 * root * alpha;
  } else {
    b0 = 1 + alpha * amplitude; b1 = -2 * cosine; b2 = 1 - alpha * amplitude;
    a0 = 1 + alpha / amplitude; a1 = -2 * cosine; a2 = 1 - alpha / amplitude;
  }
  const auto target = 2 * std::numbers::pi * frequency / sampleRate;
  const auto c = std::cos(target), s = std::sin(target), c2 = std::cos(2 * target), s2 = std::sin(2 * target);
  return std::hypot(b0 + b1 * c + b2 * c2, -b1 * s - b2 * s2) /
         std::max(1e-8, std::hypot(a0 + a1 * c + a2 * c2, -a1 * s - a2 * s2));
}

static double softLimitBoost(double value, double maximum) {
  const auto knee = maximum - 1;
  if (value <= knee) return value;
  if (value >= maximum) return maximum;
  const auto position = value - knee;
  return knee + position + position * position - position * position * position;
}

RoomEqDesign designRoomEq(const RoomEqConfiguration &configuration, std::span<const RoomEqSource> sources) {
  auto config = configuration;
  if (config.taps < 8192 || config.taps > 131072 || !std::has_single_bit(config.taps)) config.taps = 32768;
  const auto finiteRange = [](double value, double low, double high) {
    return std::isfinite(value) && value >= low && value <= high;
  };
  if (config.sampleRate < 1000 || config.sampleRate > 768000 || sources.empty() || sources.size() > 16 ||
      !finiteRange(config.smoothing, 0.02, 1) || !finiteRange(config.lowFrequency, 20, 20000) ||
      !finiteRange(config.highFrequency, config.lowFrequency, 20000) ||
      !finiteRange(config.maxBoostDb, 0, 18) || !finiteRange(config.correctionAmount, 0, 1) ||
      !finiteRange(config.directWindowMs, 1, 50) || !finiteRange(config.phaseCorrectionAmount, 0, 1) ||
      !finiteRange(config.reverbAmount, 0, 1) || !finiteRange(config.reverbWindowMs, 20, 1000) ||
      !finiteRange(config.reverbMaxFrequency, 20, 20000) || !finiteRange(config.reverbSmoothing, 0.02, 1) ||
      (config.phaseLowFrequency && !finiteRange(*config.phaseLowFrequency, 20, 20000)) ||
      (config.phaseSmoothing && !finiteRange(*config.phaseSmoothing, 0.02, 1)) ||
      static_cast<unsigned>(config.phase) > static_cast<unsigned>(RoomEqPhase::Correction))
    throw std::invalid_argument("invalid Room EQ design settings");
  const auto frequencies = logGrid(20, std::min(20000.0, config.sampleRate * 0.48));
  auto eq = std::vector<double>(frequencies.size());
  for (const auto &band : config.eqBands) {
    if (!finiteRange(band.frequency, 1, 768000) || !finiteRange(band.gain, -120, 120) ||
        !finiteRange(band.q, 0.0001, 10000) || static_cast<unsigned>(band.type) > 2)
      throw std::invalid_argument("invalid Room EQ additional EQ band");
    if (!band.enabled || band.gain == 0) continue;
    for (auto index = std::size_t{0}; index < eq.size(); ++index)
      eq[index] += gainToDb(rbjMagnitude(band, frequencies[index], config.sampleRate));
  }
  auto bins = std::vector<double>(config.taps + 1);
  for (auto bin = std::size_t{0}; bin < bins.size(); ++bin)
    bins[bin] = static_cast<double>(bin) * config.sampleRate / (config.taps * 2);
  auto result = RoomEqDesign{};
  result.filterDelaySamples = config.phase == RoomEqPhase::Minimum ? 0 : config.taps / 2;
  for (const auto &source : sources) {
    auto phaseDiagnostic = RoomEqDiagnostic{};
    phaseDiagnostic.state = source.assigned ? "notRequested" : "disabled";
    phaseDiagnostic.scale = 0;
    phaseDiagnostic.reason = config.phase == RoomEqPhase::Correction ? "impulseResponseRequired" : "fullPhaseRequired";
    auto lowDiagnostic = RoomEqDiagnostic{};
    lowDiagnostic.state = "disabled"; lowDiagnostic.scale = 0;
    lowDiagnostic.reason = !config.lowFrequencyPhaseExtension ? "notRequested" :
        !source.assigned ? "impulseResponseRequired" : "fullPhaseRequired";
    auto reverbDiagnostic = RoomEqDiagnostic{};
    reverbDiagnostic.state = config.reverbAmount == 0 ? "notRequested" :
        config.phase == RoomEqPhase::Correction ? "impulseResponseRequired" : "fullPhaseRequired";
    reverbDiagnostic.effectiveWindowMs = std::max(config.directWindowMs, config.reverbWindowMs);
    if (!source.assigned) {
      auto &channel = result.channels.emplace_back(config.taps, 0);
      channel[result.filterDelaySamples] = 1;
      result.diagnostics.phaseCorrection.push_back(std::move(phaseDiagnostic));
      result.diagnostics.lowFrequencyPhaseExtension.push_back(std::move(lowDiagnostic));
      result.diagnostics.reverbCorrection.push_back(std::move(reverbDiagnostic));
      continue;
    }
    auto measured = std::vector<double>(frequencies.size());
    auto analyses = std::vector<room_eq::ImpulseAnalysis>{};
    if (!source.impulses.empty()) {
      for (const auto &impulse : source.impulses) {
        const auto &analysis = analyses.emplace_back(room_eq::analyzeImpulse(impulse, config.sampleRate, frequencies));
        for (auto index = std::size_t{0}; index < measured.size(); ++index)
          measured[index] += analysis.magnitude[index] * analysis.magnitude[index] / source.impulses.size();
      }
      for (auto &value : measured) value = gainToDb(std::sqrt(value));
    } else {
      if (config.phase == RoomEqPhase::Correction)
        throw std::invalid_argument("Room EQ Correction requires complete impulse responses");
      if (source.response.empty()) throw std::invalid_argument("Room EQ measurement has no frequency response");
      result.supportsFullPhase = false;
      auto points = source.response;
      for (const auto &point : points)
        if (!std::isfinite(point.frequency) || point.frequency <= 0 || !std::isfinite(point.decibels))
          throw std::invalid_argument("invalid Room EQ measured frequency response");
      std::stable_sort(points.begin(), points.end(), [](const auto &a, const auto &b) { return a.frequency < b.frequency; });
      auto measuredFrequencies = std::vector<double>{}, measuredValues = std::vector<double>{};
      for (const auto &point : points) { measuredFrequencies.push_back(point.frequency); measuredValues.push_back(point.decibels); }
      measured = interpolate(measuredFrequencies, measuredValues, frequencies);
    }
    const auto smoothed = smooth(frequencies, measured, config.smoothing);
    auto levelPower = 0.0;
    auto levelCount = std::size_t{0};
    const auto high = std::min(config.highFrequency, config.sampleRate * 0.45);
    for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
      if (frequencies[index] < config.lowFrequency || frequencies[index] > high) continue;
      const auto gain = dbToGain(smoothed[index]);
      levelPower += gain * gain; ++levelCount;
    }
    const auto levelDb = gainToDb(std::sqrt(levelPower / std::max<std::size_t>(1, levelCount)));
    auto automatic = std::vector<double>(frequencies.size());
    for (auto index = std::size_t{0}; index < frequencies.size(); ++index)
      if (frequencies[index] > config.lowFrequency && frequencies[index] < high)
        automatic[index] = softLimitBoost(levelDb - measured[index], config.maxBoostDb);
    auto correction = smooth(frequencies, automatic, config.smoothing);
    for (auto index = std::size_t{0}; index < correction.size(); ++index)
      correction[index] = correction[index] * config.correctionAmount + eq[index];
    auto magnitudes = interpolate(frequencies, correction, bins);
    for (auto &value : magnitudes) value = dbToGain(value);
    auto rendered = room_eq::RenderedFilter{};
    if (config.phase == RoomEqPhase::Correction) {
      auto phaseDesign = room_eq::designPhase(analyses, source.impulses, magnitudes, bins, frequencies, config);
      rendered = std::move(phaseDesign.filter);
      result.diagnostics.phaseCorrection.push_back(std::move(phaseDesign.phase));
      result.diagnostics.lowFrequencyPhaseExtension.push_back(std::move(phaseDesign.low));
      result.diagnostics.reverbCorrection.push_back(std::move(phaseDesign.reverb));
    } else {
      const auto phase = config.phase == RoomEqPhase::Minimum ? room_eq::minimumPhase(magnitudes) : std::vector<double>(magnitudes.size());
      rendered = room_eq::renderSynthesis(magnitudes, phase, config);
      auto selected = std::span<const room_eq::ImpulseAnalysis>(analyses);
      if (config.referencePoint > 0)
        for (auto index = std::size_t{0}; index < source.impulses.size(); ++index)
          if (source.impulses[index].pointId + 1 == config.referencePoint) {
            selected = selected.subspan(index, 1); break;
          }
      for (const auto &analysis : selected)
        reverbDiagnostic.effectiveWindowMs = std::min(*reverbDiagnostic.effectiveWindowMs,
            (analysis.samples.size() - analysis.onsetIndex) * 1000.0 / config.sampleRate);
      result.diagnostics.phaseCorrection.push_back(std::move(phaseDiagnostic));
      result.diagnostics.lowFrequencyPhaseExtension.push_back(std::move(lowDiagnostic));
      result.diagnostics.reverbCorrection.push_back(std::move(reverbDiagnostic));
    }
    result.channels.push_back(std::move(rendered.taps));
    if (rendered.qualityWarning) result.qualityWarnings.emplace_back("filterAccuracy");
  }
  return result;
}

} // namespace pipetune::assets
