/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_OUTPUT_INVENTORY_H
#define PIPETUNE_PIPEWIRE_OUTPUT_INVENTORY_H

#include "pipetune/output_inventory.h"

#include <memory>

struct pw_core;

namespace pipetune {

/** Owns read-only registry bindings on the caller's PipeWire loop. */
struct PipeWireOutputInventory;

/** Releases bindings before the borrowed core is disconnected. */
struct PipeWireOutputInventoryDeleter {
  /** Destroys the observer on its PipeWire loop thread. */
  void operator()(PipeWireOutputInventory *inventory) const noexcept;
};

/** An observer with ownership independent of the borrowed PipeWire core. */
using PipeWireOutputInventoryPtr =
    std::unique_ptr<PipeWireOutputInventory, PipeWireOutputInventoryDeleter>;

/** Receives a synchronized snapshot on the core loop; the reference is borrowed. */
using OutputInventoryCallback = void (*)(const OutputInventoryResult &snapshot, void *userData);

/**
 * Observes outputs, channel layouts, and device identity changes without polling.
 * @param core Borrowed connection, which must outlive the observer.
 * @param callback Required snapshot callback; must not destroy the observer inline.
 * @param userData Borrowed callback state, which must outlive the observer.
 * @return Observer, or null if its registry binding could not be created.
 * @remarks Create and destroy on the core loop thread. Callbacks start only when
 * the loop dispatches events. Initial discovery and changes use core barriers.
 */
PipeWireOutputInventoryPtr observePipeWireOutputs(
    pw_core *core, OutputInventoryCallback callback, void *userData);

} // namespace pipetune

#endif
