/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_DSP_IDLE_H
#define PIPETUNE_DSP_IDLE_H

#include <cstdint>

namespace pipetune {

/** Disables automatic DSP suspension. */
constexpr auto kDspIdleTimeoutIgnoredMilliseconds = std::uint32_t{0};
/** Smallest selectable continuous-silence duration. */
constexpr auto kDspIdleTimeoutMinimumMilliseconds = std::uint32_t{100};
/** Largest selectable continuous-silence duration. */
constexpr auto kDspIdleTimeoutMaximumMilliseconds = std::uint32_t{5000};
/** Selectable continuous-silence duration increment. */
constexpr auto kDspIdleTimeoutStepMilliseconds = std::uint32_t{100};
/** Initial duration shown when automatic suspension is first enabled. */
constexpr auto kDspIdleTimeoutDefaultMilliseconds = std::uint32_t{1000};

/**
 * Configures automatic DSP suspension after continuous silent input.
 */
struct DspIdlePolicy {
  /** Zero disables detection; otherwise 100 through 5000 in 100 ms steps. */
  std::uint32_t timeoutMilliseconds = kDspIdleTimeoutIgnoredMilliseconds;

  /** Compares complete policy values. */
  bool operator==(const DspIdlePolicy &) const = default;
};

/**
 * Identifies current DSP work independently from preset/bypass selection.
 */
enum class DspActivity {
  /** The selected processing mode is explicit pass-through. */
  bypassed,
  /** The preset DSP is processing active input. */
  active,
  /** Silent input is being processed during the configured tail allowance. */
  draining,
  /** DSP calls are suspended and silent output is being produced. */
  sleeping
};

/**
 * Validates an automatic DSP suspension policy.
 *
 * @param policy Policy to validate.
 * @return True for ignore or a 100 through 5000 ms value in 100 ms steps.
 */
constexpr bool dspIdlePolicyIsValid(const DspIdlePolicy &policy) noexcept {
  return policy.timeoutMilliseconds == kDspIdleTimeoutIgnoredMilliseconds ||
         (policy.timeoutMilliseconds >= kDspIdleTimeoutMinimumMilliseconds &&
          policy.timeoutMilliseconds <= kDspIdleTimeoutMaximumMilliseconds &&
          policy.timeoutMilliseconds % kDspIdleTimeoutStepMilliseconds == 0);
}

/**
 * Reports whether automatic DSP suspension is enabled.
 *
 * @param policy Valid policy to inspect.
 * @return True when a non-zero timeout is configured.
 */
constexpr bool dspIdlePolicyIsEnabled(const DspIdlePolicy &policy) noexcept {
  return policy.timeoutMilliseconds != kDspIdleTimeoutIgnoredMilliseconds;
}

} // namespace pipetune

#endif
