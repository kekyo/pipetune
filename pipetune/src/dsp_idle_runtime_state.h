/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_DSP_IDLE_RUNTIME_STATE_H
#define PIPETUNE_DSP_IDLE_RUNTIME_STATE_H

#include "pipetune/dsp_idle.h"

#include <atomic>
#include <cstdint>

namespace pipetune {

/** One consistent DSP idle policy and activity observation. */
struct DspIdleRuntimeSnapshot {
  /** Policy observed with activity. */
  DspIdlePolicy policy;
  /** Activity observed with policy. */
  DspActivity activity;
  /** Opaque generation token used for conditional updates. */
  std::uint64_t token;
};

/**
 * Keeps DSP idle policy and activity consistent across control and audio threads.
 *
 * Audio-thread updates succeed only while the policy, explicit processing mode,
 * and generation observed before processing are still current. Operations are
 * lock-free and perform no allocation.
 */
class DspIdleRuntimeState final {
public:
  /**
   * Creates one runtime state.
   *
   * @param policy Initial valid idle policy.
   * @param activity Initial activity.
   */
  DspIdleRuntimeState(const DspIdlePolicy &policy,
                      DspActivity activity) noexcept
      : encoded_(encode(policy, activity, 0)) {}

  /** Returns one consistent policy, activity, and generation snapshot. */
  DspIdleRuntimeSnapshot load() const noexcept {
    return decode(encoded_.load(std::memory_order_acquire));
  }

  /**
   * Replaces policy and establishes the associated activity.
   *
   * @param policy New valid policy.
   * @param activity Activity associated with the new policy.
   * @return True when the policy value changed.
   */
  bool replacePolicy(const DspIdlePolicy &policy,
                     DspActivity activity) noexcept {
    auto current = encoded_.load(std::memory_order_acquire);
    while (true) {
      const auto observed = decode(current);
      const auto changed = observed.policy != policy;
      const auto replacement =
          encode(policy, activity, nextGeneration(current));
      if (encoded_.compare_exchange_weak(
              current, replacement, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return changed;
      }
    }
  }

  /**
   * Replaces activity and invalidates older audio-thread snapshots.
   *
   * @param activity New activity.
   */
  void replaceActivity(DspActivity activity) noexcept {
    auto current = encoded_.load(std::memory_order_acquire);
    while (true) {
      const auto observed = decode(current);
      const auto replacement =
          encode(observed.policy, activity, nextGeneration(current));
      if (encoded_.compare_exchange_weak(
              current, replacement, std::memory_order_acq_rel,
              std::memory_order_acquire)) {
        return;
      }
    }
  }

  /**
   * Replaces activity only when an earlier snapshot is still current.
   *
   * @param observed Snapshot captured before audio processing.
   * @param activity Activity resulting from that processing.
   * @return True when the conditional update succeeded.
   */
  bool tryReplaceActivity(const DspIdleRuntimeSnapshot &observed,
                          DspActivity activity) noexcept {
    auto expected = observed.token;
    const auto replacement =
        encode(observed.policy, activity, nextGeneration(observed.token));
    return encoded_.compare_exchange_strong(
        expected, replacement, std::memory_order_acq_rel,
        std::memory_order_acquire);
  }

private:
  static constexpr auto kTimeoutBits = std::uint32_t{13};
  static constexpr auto kActivityBits = std::uint32_t{2};
  static constexpr auto kActivityShift = kTimeoutBits;
  static constexpr auto kGenerationShift =
      kTimeoutBits + kActivityBits;
  static constexpr auto kTimeoutMask =
      (std::uint64_t{1} << kTimeoutBits) - 1;
  static constexpr auto kActivityMask =
      (std::uint64_t{1} << kActivityBits) - 1;
  static constexpr auto kGenerationMask =
      (std::uint64_t{1} << (64 - kGenerationShift)) - 1;
  static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
  static_assert(kDspIdleTimeoutMaximumMilliseconds <= kTimeoutMask);
  static_assert(static_cast<std::uint64_t>(DspActivity::sleeping) <=
                kActivityMask);

  static constexpr std::uint64_t
  encode(const DspIdlePolicy &policy, DspActivity activity,
         std::uint64_t generation) noexcept {
    return (generation & kGenerationMask) << kGenerationShift |
           (static_cast<std::uint64_t>(activity) & kActivityMask)
               << kActivityShift |
           (policy.timeoutMilliseconds & kTimeoutMask);
  }

  static constexpr DspIdleRuntimeSnapshot
  decode(std::uint64_t encoded) noexcept {
    return {
        .policy =
            {.timeoutMilliseconds = static_cast<std::uint32_t>(
                 encoded & kTimeoutMask)},
        .activity = static_cast<DspActivity>(
            encoded >> kActivityShift & kActivityMask),
        .token = encoded,
    };
  }

  static constexpr std::uint64_t
  nextGeneration(std::uint64_t encoded) noexcept {
    const auto generation = encoded >> kGenerationShift;
    return (generation + 1) & kGenerationMask;
  }

  std::atomic<std::uint64_t> encoded_;
};

} // namespace pipetune

#endif
