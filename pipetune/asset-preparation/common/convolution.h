/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_CONVOLUTION_H
#define PIPETUNE_ASSET_CONVOLUTION_H

#include "audio.h"
#include <cstdint>
#include <vector>

namespace pipetune::assets {

/** ETA1 convolution topology values from the EffeTune 2.13.0 asset contract. */
enum class Topology : std::uint32_t {
  mono = 1, /**< One response shared by every selected channel. */
  independent = 2, /**< One response per selected channel. */
  trueStereo = 3, /**< LL, LR, RL, RR responses for two selected channels. */
  matrix = 4 /**< Explicit input/output/response routes. */
};

/** One explicit matrix route. */
struct ConvolutionPath {
  std::uint32_t input; /**< Selected input slot. */
  std::uint32_t output; /**< Selected output slot. */
  std::uint32_t response; /**< Coefficient channel index. */
};

/** Native convolution configuration, independent of the host and backend ABI. */
struct ConvolutionConfig {
  Topology topology = Topology::mono; /**< Coefficient routing. */
  std::uint32_t headBlock = 128; /**< Latency at the convolution rate; zero is allowed. */
  std::uint32_t rateDivider = 1; /**< Audio rate divided by convolution rate. */
  std::uint32_t processingChannels = 2; /**< Actual selected processing width. */
  std::vector<ConvolutionPath> paths; /**< Routes for matrix topology only. */
};

/** An owned convolution asset ready for transfer to a native backend. */
struct ConvolutionAsset {
  ConvolutionConfig config; /**< Resolved processing configuration. */
  std::uint32_t channels = 0; /**< Number of coefficient channels. */
  std::uint32_t frames = 0; /**< Coefficient frames per channel. */
  std::uint32_t sampleRate = 0; /**< Coefficient sample rate in hertz. */
  std::uint32_t inputCount = 0; /**< Distinct matrix inputs, otherwise zero. */
  std::uint64_t footprintBytes = 0; /**< Conservative allocation bound at commit. */
  std::vector<std::uint8_t> payload; /**< ETA1 header, paths, and planar float32 PCM. */
};

/**
 * Estimates commit storage using EffeTune 2.13.0 ir-plugin-contract.js.
 * @param frames Coefficient frames.
 * @param channels Coefficient channel count.
 * @param config Resolved topology and processing width.
 * @return Payload plus the larger of staging and convolver storage bounds.
 */
std::uint64_t convolutionFootprint(std::uint32_t frames, std::uint32_t channels,
                                  const ConvolutionConfig &config);

/**
 * Encodes finite PCM into a bounded ETA1 asset.
 * @param audio Equal-length coefficient channels.
 * @param config Resolved processing configuration.
 * @return Owned bytes and complete transfer metadata.
 * @throws std::invalid_argument Invalid PCM, routes, or configuration.
 * @throws std::length_error Commit storage exceeds 32 MiB.
 */
ConvolutionAsset encodeConvolution(const Audio &audio, const ConvolutionConfig &config);

} // namespace pipetune::assets
#endif
