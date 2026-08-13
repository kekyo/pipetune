/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SAMPLE_RATE_CONVERTER_H
#define PIPETUNE_SAMPLE_RATE_CONVERTER_H

#include "pipetune/sample_rate.h"

#include <cstdint>
#include <span>
#include <vector>

namespace pipetune {

/** Rates used on the two sides of the PipeTune DSP boundary. */
struct SampleRateBridgeRates {
  /** PCM rate negotiated with PipeWire. */
  std::uint32_t streamSampleRate;
  /** Rate at which EffeTune must process the PCM. */
  std::uint32_t dspSampleRate;
  /** True when PCM conversion is required around EffeTune. */
  bool conversionRequired;
};

/**
 * Resolves independent PipeWire stream and EffeTune DSP rates.
 *
 * @param policy Configured automatic or fixed DSP-rate policy.
 * @param streamSampleRate PCM rate negotiated with PipeWire.
 * @return Resolved rates, or zero rates when either input is invalid.
 */
SampleRateBridgeRates resolveSampleRateBridgeRates(
    const SampleRatePolicy &policy, std::uint32_t streamSampleRate) noexcept;

/** Result of one bounded streaming sample-rate conversion call. */
struct SampleRateConversionResult {
  /** Frames consumed from the selected input interval. */
  std::uint32_t inputFramesUsed;
  /** Compact channel-major frames written to the output. */
  std::uint32_t outputFramesGenerated;
  /** Converter error code, or zero on success. */
  int error;
};

/**
 * Converts planar floating-point PCM without allocating during process().
 *
 * The converter owns interleaved staging buffers because libsamplerate uses
 * interleaved PCM. Its rate and history can be reset between unrelated
 * PipeWire stream formats.
 */
class PlanarSampleRateConverter final {
public:
  /**
   * Allocates one streaming converter and its bounded staging storage.
   *
   * @param channelCount Number of planar channels.
   * @param maximumFrameCount Maximum input and output frames per call.
   * @throws std::invalid_argument when either bound is zero.
   * @throws std::runtime_error when libsamplerate cannot create its state.
   */
  PlanarSampleRateConverter(std::uint32_t channelCount,
                            std::uint32_t maximumFrameCount);

  /** Releases the native converter state. */
  ~PlanarSampleRateConverter();

  /** Converters cannot be copied. */
  PlanarSampleRateConverter(const PlanarSampleRateConverter &) = delete;
  /** Converters cannot be copy-assigned. */
  PlanarSampleRateConverter &
  operator=(const PlanarSampleRateConverter &) = delete;

  /**
   * Resets stream history and selects a constant conversion ratio.
   *
   * @param inputSampleRate Input PCM rate in hertz.
   * @param outputSampleRate Output PCM rate in hertz.
   * @return Zero on success, otherwise a converter error code.
   */
  int configure(std::uint32_t inputSampleRate,
                std::uint32_t outputSampleRate) noexcept;

  /**
   * Converts a sub-interval of one compact channel-major input block.
   *
   * Generated output is compact channel-major PCM with a stride equal to
   * outputFramesGenerated. Input and output storage must not overlap.
   *
   * @param planarInput Complete compact input block.
   * @param totalInputFrames Frame stride of each input channel.
   * @param inputFrameOffset First input frame to consume.
   * @param inputFrameCount Maximum input frames to consume.
   * @param planarOutput Storage for channelCount * outputFrameCapacity values.
   * @param outputFrameCapacity Maximum frames to generate.
   * @return Consumption, generation, and converter status.
   */
  SampleRateConversionResult
  process(std::span<const float> planarInput,
          std::uint32_t totalInputFrames, std::uint32_t inputFrameOffset,
          std::uint32_t inputFrameCount, std::span<float> planarOutput,
          std::uint32_t outputFrameCapacity) noexcept;

private:
  std::uint32_t channelCount_;
  std::uint32_t maximumFrameCount_;
  void *state_;
  std::vector<float> interleavedInput_;
  std::vector<float> interleavedOutput_;
  bool configured_;
  bool passthrough_;
  double ratio_;
};

} // namespace pipetune

#endif
