/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_NUMERIC_H
#define PIPETUNE_ROOM_EQ_NUMERIC_H
#include "design.h"
#include "fft.h"

namespace pipetune::assets::room_eq {

/**
 * Creates the application's 0.01-octave design grid.
 * @param low Positive lower frequency.
 * @param high Upper frequency greater than low.
 * @return Endpoint-inclusive, uniformly spaced logarithmic grid.
 */
std::vector<double> logGrid(double low, double high);
/**
 * Interpolates values linearly in log-frequency, holding the endpoints.
 * @param frequencies Nonempty increasing positive source grid.
 * @param values Equally sized finite source values.
 * @param targets Increasing target frequencies, including zero if needed.
 * @return Interpolated target values.
 */
std::vector<double> interpolate(std::span<const double> frequencies,
    std::span<const double> values, std::span<const double> targets);
/**
 * Smooths finite values with a Gaussian in log-frequency.
 * @param frequencies Positive source frequencies.
 * @param values Equally sized source values.
 * @param sigma Gaussian width in octaves.
 * @return Smoothed values on the same grid.
 */
std::vector<double> smooth(std::span<const double> frequencies,
    std::span<const double> values, double sigma);
/**
 * Derives minimum phase by real-cepstrum factorization.
 * @param magnitudes DC-through-Nyquist nonnegative magnitudes.
 * @return Phase at every input bin, in radians.
 */
std::vector<double> minimumPhase(std::span<const double> magnitudes);

/** Realized FIR and its measured numerical agreement with the requested spectrum. */
struct RenderedFilter {
  std::vector<float> taps; /**< Float32 coefficients after the application window. */
  Spectrum actualSpectrum; /**< Measured FIR spectrum, padded to twice the tap count. */
  double maximumMagnitudeErrorDb = 0; /**< Maximum in-band magnitude error. */
  double maximumPhaseErrorRadians = 0; /**< Maximum in-band phase error; zero for Minimum. */
  bool qualityWarning = false; /**< Whether the application's accuracy thresholds are exceeded. */
};

/**
 * Synthesizes and verifies a bounded FIR from a desired magnitude and phase.
 * @param magnitudes DC-through-Nyquist values for an FFT twice the tap count.
 * @param phase Equally sized phase in radians; Linear supplies its exact quarter-cycle delay.
 * @param config Design rate, tap count, phase mode, and verification band.
 * @return Windowed coefficients and measured error.
 * @throws std::invalid_argument If a resulting coefficient is not finite.
 */
RenderedFilter renderSynthesis(std::span<const double> magnitudes,
    std::span<const double> phase, const RoomEqConfiguration &config);

} // namespace pipetune::assets::room_eq
#endif
