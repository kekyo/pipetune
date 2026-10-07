/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_GRAPH_CLOCK_H
#define PIPETUNE_PIPEWIRE_GRAPH_CLOCK_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct pw_core;

namespace pipetune {

/** Observed driver clock for one running PipeWire node. */
struct PipeWireNodeClock {
  /** Runtime node ID; meaningful only together with its serial. */
  std::uint32_t nodeId = 0;
  /** Connection generation, never persisted as a device identity. */
  std::uint64_t nodeSerial = 0;
  /** Node name as published by PipeWire. */
  std::string nodeName;
  /** Duration of one driver frame, numerator in seconds. */
  std::uint32_t rateNumerator = 0;
  /** Duration of one driver frame, denominator in seconds. */
  std::uint32_t rateDenominator = 0;
  /** Actual number of driver frames per processing cycle. */
  std::uint64_t quantum = 0;
  /** Compares the node generation and observed timing basis. */
  bool operator==(const PipeWireNodeClock &) const = default;
};

/** Owns read-only registry and public Profiler bindings on the core loop. */
struct PipeWireGraphClock;

/** Releases bindings before their borrowed core is disconnected. */
struct PipeWireGraphClockDeleter {
  /** Destroys the observer on its PipeWire loop thread. */
  void operator()(PipeWireGraphClock *observer) const noexcept;
};

/** Observer ownership independent of the borrowed connection. */
using PipeWireGraphClockPtr = std::unique_ptr<PipeWireGraphClock, PipeWireGraphClockDeleter>;

/** Receives changed snapshots on the core loop; all references are borrowed. */
using PipeWireGraphClockCallback = void (*)(const std::vector<PipeWireNodeClock> &clocks, void *userData);

/**
 * Observes the actual graph rate and quantum of running nodes without polling.
 * @param core Borrowed core, which must outlive the observer.
 * @param callback Required callback; must not destroy the observer inline.
 * @param userData Borrowed state, which must outlive the observer.
 * @return Observer, or null when the local Profiler module or registry cannot be created.
 * @remarks Calls start when the core loop dispatches events. Nodes without a
 * current profile are omitted, including idle or removed nodes. An unavailable
 * Profiler yields an empty snapshot; preferred node rates are never substituted.
 */
PipeWireGraphClockPtr observePipeWireGraphClocks(
    pw_core *core, PipeWireGraphClockCallback callback, void *userData);

} // namespace pipetune

#endif
