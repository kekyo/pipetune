/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_MEASUREMENT_RESAMPLE_H
#define PIPETUNE_MEASUREMENT_RESAMPLE_H
#include <cstdint>
#include <span>
#include <vector>

namespace pipetune::assets {

/**
 * Resamples measurement PCM with the application's 100 dB Kaiser-windowed sinc.
 * @param input Finite float32 source samples.
 * @param sourceRate Positive original sample rate in hertz.
 * @param targetRate Positive target sample rate in hertz.
 * @param radius Explicit kernel radius, or zero for the application's rate-dependent radius.
 * @return Owned float32 output with the rounded rate-scaled sample count.
 * @throws std::invalid_argument For empty input or invalid rates.
 * @remarks This measurement algorithm is independent of IR Reverb's audio resampler.
 */
std::vector<float> resampleMeasurement(std::span<const float> input, std::uint32_t sourceRate,
                                     std::uint32_t targetRate, std::uint32_t radius);

} // namespace pipetune::assets
#endif
