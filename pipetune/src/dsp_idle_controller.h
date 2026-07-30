#ifndef PIPETUNE_DSP_IDLE_CONTROLLER_H
#define PIPETUNE_DSP_IDLE_CONTROLLER_H

#include "pipetune/dsp_idle.h"

#include <cstdint>
#include <span>

namespace pipetune {

/**
 * Tracks exact-zero input and final-output quiescence on the audio thread.
 *
 * One real-time thread owns every mutation. All methods are allocation-free,
 * lock-free, and noexcept.
 */
class DspIdleController final {
public:
  DspIdleController(std::uint32_t sampleRate,
                    DspIdlePolicy policy) noexcept;

  /**
   * Observes raw planar input before volume and DSP processing.
   *
   * @return True when this complete block must be processed by the DSP.
   */
  bool observeInput(std::span<const float> planarSamples,
                    std::uint32_t channelCount,
                    std::uint32_t frameCount) noexcept;

  /**
   * Observes final planar output after DSP processing.
   *
   * @param valid False when DSP processing failed.
   * @return True when the caller should reset the DSP and enter sleep.
   */
  bool observeOutput(std::span<const float> planarSamples,
                     std::uint32_t channelCount,
                     std::uint32_t frameCount, bool valid) noexcept;

  /** Commits a previously requested sleep after DSP reset succeeds. */
  void enterSleep() noexcept;
  /** Latches a DSP reset failure for the current silent interval. */
  void rejectSleep() noexcept;

  /**
   * Wakes processing after a pipeline, rate, or policy change.
   *
   * Cumulative skipped-frame and transition counters are preserved.
   */
  void restart(std::uint32_t sampleRate,
               DspIdlePolicy policy) noexcept;

  DspIdlePolicy policy() const noexcept;
  DspIdleState state() const noexcept;
  std::uint64_t inputSilentFrames() const noexcept;
  std::uint64_t outputQuietFrames() const noexcept;
  std::uint64_t skippedFrames() const noexcept;
  std::uint64_t sleepTransitions() const noexcept;

private:
  std::uint32_t sampleRate_;
  DspIdlePolicy policy_;
  DspIdleState state_;
  std::uint64_t inputSilentFrames_;
  std::uint64_t outputQuietFrames_;
  std::uint64_t skippedFrames_;
  std::uint64_t sleepTransitions_;
  bool sleepRequested_;
  bool resetFailed_;
};

} // namespace pipetune

#endif
