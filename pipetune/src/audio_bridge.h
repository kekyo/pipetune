#ifndef PIPETUNE_AUDIO_BRIDGE_H
#define PIPETUNE_AUDIO_BRIDGE_H

#include <array>
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace pipetune {

/**
 * Transfers planar PCM between one producer and one consumer without blocking.
 *
 * Storage is allocated at construction. write() and read() perform no
 * allocation and are safe to call concurrently from their respective single
 * producer and single consumer.
 */
class PlanarAudioRing final {
public:
  /**
   * Creates an empty ring.
   *
   * @param channelCount Number of planar channels, from one through eight.
   * @param capacityFrames Maximum number of frames retained by the ring.
   * @throws std::invalid_argument if either argument is outside its range.
   */
  PlanarAudioRing(std::uint32_t channelCount, std::uint32_t capacityFrames);

  /** Rings cannot be copied. */
  PlanarAudioRing(const PlanarAudioRing &) = delete;
  /** Rings cannot be copy-assigned. */
  PlanarAudioRing &operator=(const PlanarAudioRing &) = delete;

  /**
   * Appends as many complete frames as capacity permits.
   *
   * New frames that do not fit are discarded and included in overrunFrames().
   * An incorrectly shaped buffer is rejected without changing the ring.
   *
   * @param planarSamples Contiguous channel-major input PCM.
   * @param frameCount Number of frames in each channel.
   * @param generation DSP pipeline generation that produced every frame.
   * @return Number of frames appended.
   */
  std::uint32_t write(std::span<const float> planarSamples,
                      std::uint32_t frameCount,
                      std::uint64_t generation) noexcept;

  /**
   * Removes frames and fills any unavailable tail with silence.
   *
   * Missing frames are included in underrunFrames(). An incorrectly shaped
   * buffer is rejected without changing the ring.
   *
   * @param planarSamples Contiguous channel-major output PCM.
   * @param frameCount Number of requested frames in each channel.
   * @param expectedGeneration Active DSP pipeline generation. Queued frames
   * from any other generation are consumed as silence.
   * @return Number of queued frames consumed, excluding an unavailable tail.
   */
  std::uint32_t read(std::span<float> planarSamples,
                     std::uint32_t frameCount,
                     std::uint64_t expectedGeneration) noexcept;

  /**
   * Discards all frames that were fully queued before this call.
   *
   * The consumer must be stopped while this function runs. The producer may
   * continue writing; frames committed after the discard snapshot are kept.
   * Intentional discards do not change the xrun counters.
   *
   * @return Number of queued frames discarded.
   */
  std::uint32_t discardQueuedFrames() noexcept;

  /** Returns the configured planar channel count. */
  std::uint32_t channelCount() const noexcept;
  /** Returns the frame capacity. */
  std::uint32_t capacityFrames() const noexcept;
  /** Returns the cumulative count of discarded input frames. */
  std::uint64_t overrunFrames() const noexcept;
  /** Returns the cumulative count of silence-filled output frames. */
  std::uint64_t underrunFrames() const noexcept;

private:
  std::uint32_t channelCount_;
  std::uint32_t capacityFrames_;
  std::vector<float> samples_;
  std::vector<std::uint64_t> generations_;
  alignas(64) std::atomic<std::uint64_t> readFrame_;
  alignas(64) std::atomic<std::uint64_t> writeFrame_;
  std::atomic<std::uint64_t> overrunFrames_;
  std::atomic<std::uint64_t> underrunFrames_;
};

/**
 * Smooths output discontinuities through a guaranteed silence interval.
 *
 * The object fades the last emitted sample to silence, waits for a configured
 * silence interval, and fades in only after queued PCM becomes available. Only
 * suppressed PCM advances the silence interval, so time spent in an underrun
 * cannot expire the guard before delayed frames arrive. It also smooths
 * ordinary ring-buffer underrun and recovery boundaries.
 */
class AudioTransitionSilencer final {
public:
  /**
   * Creates a transition smoother observing the supplied generation.
   *
   * @param initialGeneration Pipeline generation that is already audible.
   */
  explicit AudioTransitionSilencer(std::uint64_t initialGeneration) noexcept;

  /**
   * Starts a fresh smoothed silence interval without changing generation.
   *
   * The caller must serialize this operation with apply().
   *
   * @param silenceFrames Number of available PCM frames to suppress.
   * @param fadeFrames Number of frames in each fade-out and fade-in.
   */
  void start(std::uint32_t silenceFrames,
             std::uint32_t fadeFrames) noexcept;

  /**
   * Starts from silence after the output transport has disconnected.
   *
   * The last emitted sample is forgotten so reconnecting cannot replay it as
   * a fade-out. New PCM still fades in after the silence interval.
   *
   * @param silenceFrames Number of available PCM frames to suppress.
   * @param fadeFrames Number of frames in the subsequent fade-in.
   */
  void reset(std::uint32_t silenceFrames,
             std::uint32_t fadeFrames) noexcept;

  /**
   * Smooths a transition whenever generation or PCM availability changes.
   *
   * An incorrectly shaped buffer is left unchanged. Repeated calls with the
   * same generation continue the active transition without restarting it.
   *
   * @param planarSamples Contiguous channel-major output PCM.
   * @param channelCount Number of channels represented by the buffer.
   * @param frameCount Number of frames in each channel.
   * @param availableFrames Queued PCM frames at the beginning of the buffer.
   * @param generation Active DSP pipeline generation.
   * @param silenceFrames Available PCM frames to suppress after a generation
   * change or underrun.
   * @param fadeFrames Number of frames in each transition fade.
   * @return Number of frames adjusted in this call.
   */
  std::uint32_t apply(std::span<float> planarSamples,
                      std::uint32_t channelCount, std::uint32_t frameCount,
                      std::uint32_t availableFrames,
                      std::uint64_t generation,
                      std::uint32_t silenceFrames,
                      std::uint32_t fadeFrames) noexcept;

private:
  // Transition phases persist across arbitrary PipeWire output block sizes.
  enum class Phase {
    steady,
    fadingOut,
    silent,
    awaitingAudio,
    fadingIn,
  };

  // Captures the last audible sample and enters fade-out or direct silence.
  void beginTransition(std::uint32_t silenceFrames,
                       std::uint32_t fadeFrames) noexcept;

  std::uint64_t observedGeneration_;
  std::array<float, 8> lastOutputSamples_;
  std::array<float, 8> fadeStartSamples_;
  Phase phase_;
  std::uint32_t remainingFrames_;
  std::uint32_t pendingSilenceFrames_;
  std::uint32_t fadeFrames_;
};

} // namespace pipetune

#endif
