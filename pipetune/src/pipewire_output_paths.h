/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_OUTPUT_PATHS_H
#define PIPETUNE_PIPEWIRE_OUTPUT_PATHS_H

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct pw_core;

namespace pipetune {

/** Observed routing activity, independent of device identity resolution. */
enum class OutputPathActivity {
  /** Nodes or all required channel links have not become ready. */
  pending,
  /** The complete path is linked but paused or suspended. */
  idle,
  /** The stream and every required link to the target are running. */
  active,
  /** A node or link reports an error, or a link targets another device. */
  error
};

/** One owned distribution stream and its observed physical output path. */
struct PipeWireOutputPath {
  /** Saved output identifier stamped on the distribution stream. */
  std::string outputId;
  /** Distribution stream generation; distinguishes graph replacements. */
  std::uint64_t streamSerial = 0;
  /** Target device generation requested by this stream. */
  std::uint64_t targetSerial = 0;
  /** Currently observed target ID, or UINT32_MAX while unavailable. */
  std::uint32_t targetNodeId = UINT32_MAX;
  /** Activity established from node and channel link states. */
  OutputPathActivity activity = OutputPathActivity::pending;
  /** Midpoint of the reported downstream latency range in nanoseconds.
   * Absent without a running path, valid latency parameters, and actual clock.
   * This is neither a private compensation buffer size nor an acoustic measurement.
   */
  std::optional<double> reportedLatencyNanoseconds;
  /** Compares connection generations, activity, and the current estimate basis. */
  bool operator==(const PipeWireOutputPath &) const = default;
};

/** Owns read-only topology, latency, and clock observations on the core loop. */
struct PipeWireOutputPaths;

/** Releases all bindings before the borrowed core is disconnected. */
struct PipeWireOutputPathsDeleter {
  /** Destroys the observer on its PipeWire loop thread. */
  void operator()(PipeWireOutputPaths *observer) const noexcept;
};

/** Ownership independent of the borrowed connection. */
using PipeWireOutputPathsPtr = std::unique_ptr<PipeWireOutputPaths, PipeWireOutputPathsDeleter>;

/** Receives changed snapshots on the core loop; references are borrowed. */
using PipeWireOutputPathsCallback = void (*)(const std::vector<PipeWireOutputPath> &paths, void *userData);

/**
 * Observes the distribution streams belonging to one PipeTune public input.
 * @param core Borrowed core, which must outlive the observer.
 * @param publicInputName Owning public input name, copied by the observer.
 * @param callback Required callback; must not destroy the observer inline.
 * @param userData Borrowed callback state, which must outlive the observer.
 * @return Observer, or null if the registry could not be created.
 * @remarks Snapshots may briefly contain multiple generations during replacement;
 * callers must not merge them into a single active path. No polling is used.
 * Missing Profiler support leaves latency unknown without suppressing activity.
 */
PipeWireOutputPathsPtr observePipeWireOutputPaths(pw_core *core,
    const std::string &publicInputName, PipeWireOutputPathsCallback callback, void *userData);

} // namespace pipetune

#endif
