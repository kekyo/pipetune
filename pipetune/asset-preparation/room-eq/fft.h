/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_FFT_H
#define PIPETUNE_ROOM_EQ_FFT_H

#include <span>
#include <vector>

namespace pipetune::assets::room_eq {

/** Nonredundant real-input spectrum, with double precision accumulators. */
struct Spectrum {
  /** DC through Nyquist real components. */
  std::vector<double> real;
  /** DC through Nyquist imaginary components. */
  std::vector<double> imag;
};

/**
 * Transform real samples using the measurement application's float32 twiddles.
 * @param input Power-of-two input of at least four samples.
 * @return Nonredundant spectrum without forward normalization.
 * @throws std::invalid_argument If the input length is unsupported.
 */
Spectrum realTransform(std::span<const double> input);

/**
 * Reconstruct real samples, dividing the inverse transform by its length.
 * @param spectrum Matching real/imaginary DC-through-Nyquist arrays.
 * @return Twice (spectrum.real.size() - 1) real samples.
 * @throws std::invalid_argument If the dimensions are inconsistent.
 */
std::vector<double> inverseRealTransform(const Spectrum &spectrum);

} // namespace pipetune::assets::room_eq
#endif
