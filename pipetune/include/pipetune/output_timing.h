/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_TIMING_H
#define PIPETUNE_OUTPUT_TIMING_H

#include <cstdint>
#include <optional>
#include <string>

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

/** Runtime timing for one resolved output; never persisted as routing settings. */
struct OutputTimingState {
  /** Saved output identifier. */
  std::string outputId;
  /** Current physical device generation, matching the live inventory. */
  std::uint64_t nodeSerial = 0;
  /** Activity established from nodes and all required channel links. */
  OutputPathActivity activity = OutputPathActivity::pending;
  /** Midpoint of reported downstream latency, absent without a valid active path. */
  std::optional<double> reportedLatencyNanoseconds;
  /** Estimated additional delay relative to the slowest resolved output.
   * Absent until every resolved output has a valid active path and timing basis.
   * This does not measure the private compensation buffer or acoustic arrival.
   */
  std::optional<double> estimatedCompensationNanoseconds;
  /** Compares the target generation, activity, and estimates. */
  bool operator==(const OutputTimingState &) const = default;
};

} // namespace pipetune

#endif
