#ifndef PIPETUNE_PIPEWIRE_VOLUME_SMOOTHER_H
#define PIPETUNE_PIPEWIRE_VOLUME_SMOOTHER_H

#include <spa/pod/pod.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <span>

namespace pipetune {

/**
 * Applies desktop sink volume changes as click-free planar PCM gain ramps.
 *
 * PipeWire parameter updates are published from the main loop and consumed
 * without locks or allocation by the realtime process callback.
 */
class PipeWireVolumeSmoother {
public:
  /**
   * Creates a unity-gain volume processor.
   *
   * @param channelCount Number of planar PCM channels to process.
   */
  explicit PipeWireVolumeSmoother(std::uint32_t channelCount) noexcept;

  /**
   * Updates the target gain from a PipeWire SPA_PARAM_Props value.
   *
   * Effective channel volumes are preferred over software-volume details.
   * Partial parameters preserve all state not present in the update.
   *
   * @param parameter PipeWire volume properties.
   * @param rampSamples Positive number of samples in the next gain ramp.
   * @return True when a supported volume property changed.
   */
  bool update(const spa_pod *parameter,
              std::uint32_t rampSamples) noexcept;

  /**
   * Applies the current gain or gain transition in place.
   *
   * @param planarSamples Channel-major floating-point PCM.
   * @param frameCount Number of frames per channel.
   */
  void process(std::span<float> planarSamples,
               std::uint32_t frameCount) noexcept;

private:
  static constexpr auto kMaximumChannels = std::uint32_t{8};

  std::uint32_t channelCount_;
  float masterVolume_;
  std::array<float, kMaximumChannels> channelVolumes_;
  bool muted_;
  std::atomic<std::uint32_t> publicationSequence_;
  std::array<std::atomic<float>, kMaximumChannels> publishedGains_;
  std::atomic<std::uint32_t> publishedRampSamples_;
  std::uint32_t appliedSequence_;
  std::array<float, kMaximumChannels> currentGains_;
  std::array<float, kMaximumChannels> targetGains_;
  std::array<float, kMaximumChannels> gainSteps_;
  std::uint32_t remainingRampSamples_;
};

} // namespace pipetune

#endif
