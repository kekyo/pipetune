/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_INVENTORY_H
#define PIPETUNE_OUTPUT_INVENTORY_H

#include "pipetune/output_configuration.h"
#include "pipetune/output_volume.h"

namespace pipetune {

/** A read-only snapshot of all eligible audio sinks. */
struct OutputInventoryResult {
  /** Outputs including devices whose layout cannot be selected. */
  std::vector<AvailableOutput> outputs;
  /** Empty on success; an enumeration failure is never an empty success. */
  std::string error;
  /** Reported controls for current outputs; absent controls remain unknown. */
  std::vector<OutputVolumeState> volumes = {};
};

/**
 * Queries the current PipeWire session without requiring a PipeTune daemon.
 * @return A synchronized snapshot, or a connection/enumeration diagnostic.
 * @remarks Does not activate devices, change profiles, or change routing.
 * Uses the normal PipeWire connection environment and a bounded event loop.
 */
OutputInventoryResult queryAvailableOutputs();

/**
 * Formats every output and its ordered, one-based physical channel labels.
 * @param outputs Current inventory, including unsupported layouts.
 * @param json True for machine-readable JSON, false for display text.
 * @return Formatted inventory, or empty if JSON allocation fails.
 */
std::string formatOutputInventory(std::span<const AvailableOutput> outputs, bool json);

} // namespace pipetune

#endif
