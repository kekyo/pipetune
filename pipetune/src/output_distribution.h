/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_DISTRIBUTION_H
#define PIPETUNE_OUTPUT_DISTRIBUTION_H

#include "pipetune/output_configuration.h"
#include "pipetune/sample_rate.h"

namespace pipetune {

/** PipeWire module arguments for one fixed DSP output layout. */
struct OutputDistributionArguments {
  /** Complete escaped JSON, including an explicit empty rule list if disconnected. */
  std::string arguments;
  /** Validation or encoding error, or empty on success. */
  std::string error;
};

/**
 * Maps mixed AUX slots to uniquely resolved device channels without fallback.
 * @param nodeName Private distribution sink name.
 * @param publicInputName Public playback target retained during stream rebuilds.
 * @param configuration Valid saved routing, including disabled/reserved slots.
 * @param inventory Current device descriptions and runtime object generations.
 * @return combine-stream arguments with latency compensation, or an error.
 */
OutputDistributionArguments makeOutputDistributionArguments(
    const std::string &nodeName, const std::string &publicInputName, const OutputConfiguration &configuration,
    std::span<const AvailableOutput> inventory);

/**
 * Publishes a numbered direct input connected to the shared output mixer.
 * @param publicInputName Stable processed-input name; the direct name adds `.direct`.
 * @param channels Fixed output slot count, including disconnected slots.
 * @param policy Common sample-rate preference and graph enforcement.
 * @return Loopback module arguments, or a validation/encoding error.
 */
OutputDistributionArguments makeDirectOutputArguments(
    const std::string &publicInputName, std::uint32_t channels,
    const SampleRatePolicy &policy);

} // namespace pipetune

#endif
