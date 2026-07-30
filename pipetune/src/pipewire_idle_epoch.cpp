#include "pipewire_idle_epoch.h"

namespace pipetune {

PipeWireIdleEpoch::PipeWireIdleEpoch() noexcept
    : pausedEpochLatched_(false), dspResetPending_(false),
      waitingForCapture_(false) {}

bool PipeWireIdleEpoch::observeStreamStates(bool capturePaused,
                                            bool playbackPaused) noexcept {
  if (!capturePaused || !playbackPaused) {
    pausedEpochLatched_ = false;
    return false;
  }
  if (pausedEpochLatched_) {
    return false;
  }

  pausedEpochLatched_ = true;
  waitingForCapture_.store(true, std::memory_order_release);
  dspResetPending_.store(true, std::memory_order_release);
  return true;
}

bool PipeWireIdleEpoch::takeDspResetRequest() noexcept {
  return dspResetPending_.exchange(false, std::memory_order_acq_rel);
}

bool PipeWireIdleEpoch::playbackShouldTreatMissingAsGap() const noexcept {
  return waitingForCapture_.load(std::memory_order_acquire);
}

void PipeWireIdleEpoch::captureFramesQueued() noexcept {
  waitingForCapture_.store(false, std::memory_order_release);
}

} // namespace pipetune
