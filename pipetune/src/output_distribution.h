/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_DISTRIBUTION_H
#define PIPETUNE_OUTPUT_DISTRIBUTION_H

#include "pipetune/output_configuration.h"

namespace pipetune {

/** PipeWire module arguments for one fixed DSP output layout. */
struct OutputDistributionArguments {
  /** Complete escaped JSON, including an explicit empty rule list if disconnected. */
  std::string arguments;
  /** Validation or encoding error, or empty on success. */
  std::string error;
};

/**
 * Maps DSP AUX slots to uniquely resolved device channels without fallback.
 * @param nodeName Private distribution sink name.
 * @param configuration Valid saved routing, including disabled/reserved slots.
 * @param inventory Current device descriptions and runtime object generations.
 * @return combine-stream arguments with latency compensation, or an error.
 */
OutputDistributionArguments makeOutputDistributionArguments(
    const std::string &nodeName, const OutputConfiguration &configuration,
    std::span<const AvailableOutput> inventory);

} // namespace pipetune

#endif
