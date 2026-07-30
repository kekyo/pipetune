#ifndef PIPETUNE_DSP_IDLE_H
#define PIPETUNE_DSP_IDLE_H

namespace pipetune {

/**
 * Selects how the final DSP output must settle before processing can sleep.
 */
enum class DspIdlePolicy {
  /** Accept output at or below -150 dBFS after exact-zero input. */
  conservative,
  /** Require mathematically exact-zero input and output. */
  exact
};

/**
 * Describes the real-time DSP idle controller state.
 */
enum class DspIdleState {
  /** Recent input activity requires ordinary processing. */
  active,
  /** Exact-zero input is being processed to preserve DSP tail output. */
  draining,
  /** Exact-zero input is monitored without invoking the DSP pipeline. */
  sleeping
};

} // namespace pipetune

#endif
