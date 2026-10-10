/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_VOLUME_H
#define PIPETUNE_OUTPUT_VOLUME_H

#include <cstdint>
#include <optional>
#include <vector>

namespace pipetune {

/** Read-only device volume reports, separate from the logical master control. */
struct OutputVolumeState {
  /** Current PipeWire object generation; never persisted or matched by node ID. */
  std::uint64_t nodeSerial = 0;
  /** Reported device mute; absent when the device does not expose this control. */
  std::optional<bool> muted = {};
  /** Reported scalar volume; absent when unavailable. One means unity gain. */
  std::optional<float> volume = {};
  /**
   * Reported effective channel gains, including hardware/software attenuation.
   * Empty means unavailable. Values are linear and must not be multiplied by
   * softVolumes, which describe the software portion of these same gains.
   */
  std::vector<float> channelVolumes = {};
  /** Compares the connection generation and all reported controls. */
  bool operator==(const OutputVolumeState &) const = default;
};

} // namespace pipetune

#endif
