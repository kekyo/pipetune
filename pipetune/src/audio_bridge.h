#ifndef PIPETUNE_AUDIO_BRIDGE_H
#define PIPETUNE_AUDIO_BRIDGE_H

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
   * @return Number of frames appended.
   */
  std::uint32_t write(std::span<const float> planarSamples,
                      std::uint32_t frameCount) noexcept;

  /**
   * Removes frames and fills any unavailable tail with silence.
   *
   * Missing frames are included in underrunFrames(). An incorrectly shaped
   * buffer is rejected without changing the ring.
   *
   * @param planarSamples Contiguous channel-major output PCM.
   * @param frameCount Number of requested frames in each channel.
   * @return Number of frames copied from queued audio, excluding silence.
   */
  std::uint32_t read(std::span<float> planarSamples,
                     std::uint32_t frameCount) noexcept;

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
  alignas(64) std::atomic<std::uint64_t> readFrame_;
  alignas(64) std::atomic<std::uint64_t> writeFrame_;
  std::atomic<std::uint64_t> overrunFrames_;
  std::atomic<std::uint64_t> underrunFrames_;
};

} // namespace pipetune

#endif
