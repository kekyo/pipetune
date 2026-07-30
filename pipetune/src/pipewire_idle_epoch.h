#ifndef PIPETUNE_PIPEWIRE_IDLE_EPOCH_H
#define PIPETUNE_PIPEWIRE_IDLE_EPOCH_H

#include <atomic>

namespace pipetune {

/**
 * Coordinates one complete PipeWire graph-idle interval across callbacks.
 *
 * observeStreamStates() is owned by the main-loop thread. The remaining
 * methods communicate with the capture and playback real-time threads without
 * allocation or locking.
 */
class PipeWireIdleEpoch final {
public:
  /** Creates an active epoch coordinator with no pending work. */
  PipeWireIdleEpoch() noexcept;

  /**
   * Observes the latest capture and playback stream states.
   *
   * @param capturePaused Whether the capture stream is paused.
   * @param playbackPaused Whether the playback stream is paused.
   * @return True once when both streams enter one paused interval.
   */
  bool observeStreamStates(bool capturePaused, bool playbackPaused) noexcept;

  /**
   * Takes the pending DSP reset request on the capture real-time thread.
   *
   * @return True once for each observed paused interval.
   */
  bool takeDspResetRequest() noexcept;

  /**
   * Reports whether playback must emit GAP until fresh capture is queued.
   *
   * @return True between a paused interval and the first resumed capture data.
   */
  bool playbackShouldTreatMissingAsGap() const noexcept;

  /**
   * Publishes that the first resumed capture frames have been queued.
   *
   * This must be called only after the frames are committed to the shared
   * audio ring.
   */
  void captureFramesQueued() noexcept;

private:
  bool pausedEpochLatched_;
  std::atomic<bool> dspResetPending_;
  std::atomic<bool> waitingForCapture_;
};

} // namespace pipetune

#endif
