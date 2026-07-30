#ifndef PIPETUNE_PIPEWIRE_VOLUME_STATE_H
#define PIPETUNE_PIPEWIRE_VOLUME_STATE_H

#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/pod/pod.h>

#include <array>
#include <cstdint>
#include <span>

namespace pipetune {

/**
 * Stores the effective volume controls reported by one PipeWire node.
 *
 * channelVolumes contains the effective linear gain. It must not be
 * multiplied by SPA_PROP_volume or SPA_PROP_softVolumes.
 */
struct PipeWireVolumeState {
  /** Effective linear volume for each channel. */
  std::array<float, SPA_AUDIO_MAX_CHANNELS> channelVolumes = {};
  /** SPA audio position corresponding to each channel when known. */
  std::array<std::uint32_t, SPA_AUDIO_MAX_CHANNELS> channelMap = {};
  /** Number of valid entries in channelVolumes. Zero means unknown. */
  std::uint32_t channelCount = 0;
  /** Number of valid entries in channelMap. */
  std::uint32_t channelMapCount = 0;
  /** Effective mute state. */
  bool muted = false;
  /** True after an effective mute property has been observed. */
  bool muteKnown = false;
};

/**
 * Reports the result of merging a SPA_PARAM_Props value.
 */
struct PipeWireVolumeMergeResult {
  /** True when every relevant property in the parameter was valid. */
  bool valid;
  /** True when the destination state changed. */
  bool changed;
  /** True when effective channel volumes were present. */
  bool volumePresent;
};

/**
 * Merges effective PipeWire volume properties into an existing state.
 *
 * Missing properties preserve their previous values. Malformed relevant
 * properties reject the complete update without modifying the destination.
 *
 * @param parameter SPA_TYPE_OBJECT_Props parameter to parse.
 * @param state State updated after successful validation.
 * @return Validation, change, and effective-volume presence flags.
 */
PipeWireVolumeMergeResult mergePipeWireVolumeState(
    const spa_pod *parameter, PipeWireVolumeState &state) noexcept;

/**
 * Builds a writable SPA_PARAM_Props value from an effective volume state.
 *
 * @param builder Builder owning the returned POD storage.
 * @param state Complete effective volume state.
 * @return Built parameter, or nullptr when no channel volume is available.
 */
spa_pod *buildPipeWireVolumeParameter(
    spa_pod_builder &builder,
    const PipeWireVolumeState &state) noexcept;

/**
 * Maps a physical output's volume state to PipeTune's virtual sink layout.
 *
 * @param physical Physical output state.
 * @param virtualChannelMap PipeTune virtual sink channel positions.
 * @return Complete virtual-sink control state.
 */
PipeWireVolumeState mapPhysicalVolumeToVirtual(
    const PipeWireVolumeState &physical,
    std::span<const std::uint32_t> virtualChannelMap) noexcept;

/**
 * Maps a virtual-sink request onto a physical output.
 *
 * Matching channel positions receive their requested values. Physical
 * channels absent from the virtual layout keep their relative balance while
 * following the virtual master-volume ratio.
 *
 * @param requestedVirtual New virtual-sink control state.
 * @param previousVirtual Previously published virtual-sink state.
 * @param physical Current physical output state.
 * @return Requested effective physical output state.
 */
PipeWireVolumeState mapVirtualVolumeToPhysical(
    const PipeWireVolumeState &requestedVirtual,
    const PipeWireVolumeState &previousVirtual,
    const PipeWireVolumeState &physical) noexcept;

/**
 * Compares two effective volume states with floating-point tolerance.
 *
 * @param left First state.
 * @param right Second state.
 * @return True when all effective controls are equivalent.
 */
bool pipeWireVolumeStatesEquivalent(
    const PipeWireVolumeState &left,
    const PipeWireVolumeState &right) noexcept;

} // namespace pipetune

#endif
