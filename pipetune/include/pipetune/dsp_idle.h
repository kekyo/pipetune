#ifndef PIPETUNE_DSP_IDLE_H
#define PIPETUNE_DSP_IDLE_H

#include <optional>
#include <string_view>

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

/**
 * Returns the default DSP idle policy.
 */
DspIdlePolicy defaultDspIdlePolicy() noexcept;

/**
 * Returns the stable configuration and protocol name for a policy.
 *
 * @param policy DSP idle policy.
 * @return `conservative`, `exact`, or an empty view for an invalid value.
 */
std::string_view dspIdlePolicyName(DspIdlePolicy policy) noexcept;

/**
 * Parses a stable DSP idle policy name.
 *
 * @param name Candidate configuration or protocol name.
 * @return Parsed policy, or no value for an unsupported name.
 */
std::optional<DspIdlePolicy>
parseDspIdlePolicyName(std::string_view name) noexcept;

/**
 * Returns the stable protocol name for a DSP idle state.
 *
 * @param state Current DSP idle controller state.
 * @return `active`, `draining`, `sleeping`, or an empty view when invalid.
 */
std::string_view dspIdleStateName(DspIdleState state) noexcept;

} // namespace pipetune

#endif
