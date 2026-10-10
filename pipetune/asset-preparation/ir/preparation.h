/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_IR_PREPARATION_H
#define PIPETUNE_IR_PREPARATION_H

#include "common/convolution.h"
#include <optional>
#include <string_view>

namespace pipetune::assets {

/** Resolved IR configuration, before source-rate conversion. */
struct IrConfiguration {
  ConvolutionConfig convolution; /**< Native routing and latency. */
  std::uint32_t assetChannels; /**< Coefficient channels retained at emission. */
  std::uint32_t sampleRate; /**< Required source PCM rate after conversion. */
};

/** Parameters of the application's host-side IR preparation. */
struct IrOptions {
  bool directCut = false; /**< Remove the detected direct arrival. */
  double cutOffsetMs = 0; /**< Offset from onset, from -20 through 50 ms. */
  double decayPercent = 100; /**< Decay duration scale, from 10 through 400 percent. */
  double trimPercent = 100; /**< Retained duration, from 1 through 100 percent. */
};

/** Observable analysis of prepared coefficient channels. */
struct IrAnalysis {
  std::optional<double> rt60Seconds; /**< Decay regression, absent without sufficient decay data. */
  double peakDb = -120; /**< Peak coefficient magnitude in dB. */
  double l1GainUpperBound = 0; /**< Largest routed sum of absolute coefficients. */
  std::vector<std::uint32_t> sampleFrames; /**< Starting frame of each analysis bin. */
  std::vector<float> envelope; /**< Peak magnitude in each analysis bin. */
  std::vector<float> edcDb; /**< Integrated energy decay at each bin start. */
};

/** Prepared asset and diagnostics, without host or file ownership. */
struct PreparedIr {
  ConvolutionAsset asset; /**< Owned, capacity-bounded ETA1 payload. */
  IrAnalysis original; /**< Analysis after initial normalization and direct cut. */
  IrAnalysis analysis; /**< Analysis of the final emitted channels. */
  std::uint32_t onsetFrame = 0; /**< Detected direct arrival in original PCM. */
  std::uint32_t leadingSilenceFrames = 0; /**< Removed initial digital silence. */
  std::uint32_t sourceStartFrame = 0; /**< First original frame retained after direct cut. */
  bool trimmed = false; /**< Duration was reduced by the Trim parameter. */
  bool capacityLimited = false; /**< Commit capacity additionally shortened the asset. */
  std::vector<float> initialGains; /**< First float32 normalization gains. */
  std::vector<float> finalGains; /**< Second float32 normalization gains. */
};

/**
 * Resolves EffeTune's channel mode, latency, and convolution-rate rules.
 * @param sampleRate Engine rate in hertz.
 * @param channelCount Decoded source width.
 * @param processingChannels Actual selected audio width.
 * @param channelMode auto, mono, indep, true, or multi.
 * @param headBlock 0, 128, 256, 512, or 1024.
 * @param convolutionRate auto, full, half, or quarter.
 * @return A configuration suitable for source conversion and preparation.
 * @throws std::invalid_argument Unavailable channels or unsupported settings.
 */
IrConfiguration resolveIrConfiguration(std::uint32_t sampleRate, std::uint32_t channelCount,
    std::uint32_t processingChannels, std::string_view channelMode,
    std::uint32_t headBlock, std::string_view convolutionRate);

/**
 * Reproduces prepareIr followed by emitPreparedIr in EffeTune 2.13.0.
 * @param audio Original channels, already converted to configuration.sampleRate.
 * @param configuration Resolved routing and emission width.
 * @param options Direct cut, decay, and duration controls.
 * @return Emitted asset, source-frame information, gains, and analysis.
 * @throws std::invalid_argument Invalid PCM or controls.
 * @throws std::length_error Even the shortest asset exceeds its commit budget.
 * @remarks Normalization precedes emission channel selection and capacity fading.
 */
PreparedIr prepareIr(const Audio &audio, const IrConfiguration &configuration, const IrOptions &options);

} // namespace pipetune::assets
#endif
