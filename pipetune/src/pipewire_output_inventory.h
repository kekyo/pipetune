/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_OUTPUT_INVENTORY_H
#define PIPETUNE_PIPEWIRE_OUTPUT_INVENTORY_H

#include "pipetune/output_inventory.h"

#include <memory>
#include <optional>
#include <cstdint>
#include <vector>
#include <span>
#include <string>

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

/**
 * Reports losses of the default-output policy observed on this connection.
 * @param inventory Live observer, accessed on its core loop thread.
 * @return Monotonic generation, initially zero. Changes trigger a snapshot.
 * @remarks A removed default metadata object invalidates the policy's graph
 * configuration even when the physical output inventory has not changed.
 */
std::uint64_t pipeWireOutputPolicyGeneration(const PipeWireOutputInventory &inventory) noexcept;

/** Requests a card profile on the PipeWire loop without asking WirePlumber to save it. */
std::string setPipeWireOutputProfile(
    PipeWireOutputInventory &inventory, const OutputProfileSelection &selection);

/** Returns the advertised sink Node count for a card profile, if known. */
std::optional<std::uint32_t> pipeWireOutputProfileNodeCount(
    const PipeWireOutputInventory &inventory, const OutputProfileSelection &selection);

/** Active input route and its complete Props object before a card profile change. */
struct PipeWireInputRouteSnapshot {
  /** Route index within the card. */
  std::int32_t index = -1;
  /** Logical input device index within the card. */
  std::int32_t device = -1;
  /** Serialized SPA Props POD, or empty when no properties are reported. */
  std::vector<std::uint8_t> props;
  /** Compares input settings independently of the active card profile. */
  bool operator==(const PipeWireInputRouteSnapshot &) const = default;
};

/** Captures the current input settings of one physical card. */
std::vector<PipeWireInputRouteSnapshot> pipeWireInputRoutes(
    const PipeWireOutputInventory &inventory, const OutputCardIdentity &card);

/** Restores input route and Props with save=false after a card profile change. */
std::string restorePipeWireInputRoutes(PipeWireOutputInventory &inventory,
    const OutputCardIdentity &card, std::span<const PipeWireInputRouteSnapshot> routes);

} // namespace pipetune

#endif
