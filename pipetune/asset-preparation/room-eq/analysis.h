/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_ANALYSIS_H
#define PIPETUNE_ROOM_EQ_ANALYSIS_H
#include "design.h"

namespace pipetune::assets::room_eq {

/** One measurement resampled and scaled for magnitude and phase design. */
struct ImpulseAnalysis {
  std::vector<float> samples; /**< Finite samples at the design rate. */
  std::size_t onsetIndex = 0; /**< Onset rounded to the design rate. */
  std::size_t fftSize = 0; /**< Smallest power of two covering the samples. */
  std::vector<double> spectrumMagnitude; /**< Full nonredundant spectrum magnitude. */
  std::vector<double> magnitude; /**< RMS magnitude per logarithmic frequency cell. */
  std::vector<float> groupDelaySamples; /**< Optional full-length aligned consensus for delay guards. */
  std::size_t groupDelayOnsetIndex = 0; /**< Consensus onset within groupDelaySamples. */
};

/**
 * Normalizes a measurement and analyzes its magnitude on the design grid.
 * @param impulse Original measurement PCM, onset, and reference scale.
 * @param sampleRate Design sample rate in hertz.
 * @param frequencies Positive increasing logarithmic frequency grid with at least two points.
 * @return Independently owned, rate-adjusted measurement analysis.
 * @throws std::invalid_argument For invalid measurement metadata or nonfinite PCM.
 */
ImpulseAnalysis analyzeImpulse(const RoomEqImpulse &impulse, std::uint32_t sampleRate,
                               std::span<const double> frequencies);

} // namespace pipetune::assets::room_eq
#endif
