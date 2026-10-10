/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_GROUP_DELAY_H
#define PIPETUNE_ROOM_EQ_GROUP_DELAY_H
#include "fft.h"
#include <cstdint>

namespace pipetune::assets::room_eq {

/** Independent total, minimum-phase, and excess group-delay analysis. */
struct GroupDelay {
  std::size_t fftSize = 0; /**< FFT size, including cepstral padding. */
  Spectrum spectrum; /**< Owned full measurement spectrum when available. */
  std::vector<std::uint8_t> valid; /**< Whether each grid point has a usable magnitude. */
  std::vector<double> totalMs; /**< Total onset-relative delay in milliseconds. */
  std::vector<double> minimumMs; /**< Minimum-phase delay in milliseconds. */
  std::vector<double> excessMs; /**< Total minus minimum-phase delay in milliseconds. */
};

/** Smoothed float32 values used by the application's realization diagnostics. */
struct DisplayGroupDelay {
  std::vector<std::uint8_t> valid; /**< Usable grid entries. */
  std::vector<float> total; /**< Smoothed total delay in milliseconds. */
  std::vector<float> minimum; /**< Smoothed minimum-phase delay in milliseconds. */
  std::vector<float> excess; /**< Smoothed excess delay in milliseconds. */
};

/**
 * Measures group delay from H and FFT(n*h[n]) with fourfold cepstral padding.
 * @param samples Finite time-domain samples.
 * @param alignmentSamples Onset or known FIR center to remove, in samples.
 * @param sampleRate Sample rate in hertz.
 * @param frequencies Increasing target frequencies in hertz.
 * @param minimumFftSize Minimum padded FFT size.
 * @return Delay components on the requested grid, retaining validity gaps.
 */
GroupDelay analyzeGroupDelay(std::span<const double> samples, double alignmentSamples,
    std::uint32_t sampleRate, std::span<const double> frequencies, std::size_t minimumFftSize);

/**
 * Adds cascaded responses' delays, retaining only jointly valid grid entries.
 * @param first First response on a shared grid.
 * @param second Second response on the same grid.
 * @return Combined delay components.
 */
GroupDelay combineGroupDelay(const GroupDelay &first, const GroupDelay &second);

/**
 * Averages usable points' group delays on one shared grid.
 * @param analyses Nonempty set of equal-grid analyses.
 * @return Pointwise mean of every available observation.
 */
GroupDelay averageGroupDelay(std::span<const GroupDelay> analyses);

/**
 * Smooths individual validity runs without interpolating across missing observations.
 * @param analysis Delay components on the requested grid.
 * @param frequencies Corresponding positive frequencies.
 * @param smoothing Gaussian width in octaves.
 * @return Float32 diagnostic/display values.
 */
DisplayGroupDelay smoothGroupDelay(const GroupDelay &analysis,
    std::span<const double> frequencies, double smoothing);

/**
 * Integrates absolute group delay separately within each valid run.
 * @param frequencies Increasing frequency grid in hertz.
 * @param delaysSeconds Onset-relative delays, in seconds.
 * @param valid Usable observations.
 * @param wrappedPhase Original phase whose nearest-1-kHz value anchors each run.
 * @return Integrated phase in radians, keeping invalid entries unchanged.
 */
std::vector<double> integrateGroupDelayPhase(std::span<const double> frequencies,
    std::span<const double> delaysSeconds, std::span<const std::uint8_t> valid,
    std::span<const double> wrappedPhase);

} // namespace pipetune::assets::room_eq
#endif
