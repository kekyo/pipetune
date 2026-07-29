#ifndef PIPETUNE_OUTPUT_DEVICE_TRACKER_H
#define PIPETUNE_OUTPUT_DEVICE_TRACKER_H

#include "pipetune/sample_rate.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pipetune {

/**
 * Identifies why an output is currently selected.
 */
enum class OutputSelectionReason {
  /** No usable physical output is available. */
  unavailable,
  /** No preference exists, so the physical system default is selected. */
  systemDefault,
  /** The available user-preferred output is selected. */
  preferred,
  /** The preference is unavailable and a physical fallback is selected. */
  fallback
};

/**
 * Describes one PipeWire Audio/Sink candidate.
 */
struct OutputDevice {
  /** PipeWire global identifier. */
  std::uint32_t id;
  /** Stable node.name used as target.object. */
  std::string name;
  /** Human-readable node description, falling back to node.name. */
  std::string description;
  /** Session-manager preference; larger values are preferred. */
  std::int32_t priority;
  /** True for software or other virtual sinks. */
  bool virtualNode;
  /** PipeWire EnumFormat sample-rate capabilities. */
  SampleRateCapabilities sampleRateCapabilities = {};
  /** Current physical Format rate, or zero while inactive. */
  std::uint32_t activeSampleRate = 0;
};

/**
 * Selects a non-PipeTune physical output as registry state changes.
 *
 * An available user preference wins. Otherwise the tracker uses the remembered
 * physical system default and retains that fallback until it changes or
 * disappears. A missing preference remains configured so that hotplug can
 * restore it automatically.
 */
class OutputDeviceTracker final {
public:
  /**
   * Creates an empty tracker.
   *
   * @param excludedNodeName PipeTune virtual sink name.
   * @param preferredTarget User-preferred node.name, or empty for automatic
   * default-device tracking.
   */
  OutputDeviceTracker(std::string excludedNodeName,
                      std::string preferredTarget);

  /**
   * Adds or replaces an Audio/Sink description.
   *
   * @param device Registry device state.
   * @return True when selectedTarget() changed.
   */
  bool updateDevice(OutputDevice device);

  /**
   * Removes a registry object.
   *
   * @param id Removed PipeWire global identifier.
   * @return True when selectedTarget() changed.
   */
  bool removeDevice(std::uint32_t id);

  /**
   * Replaces one device's normalized EnumFormat rate capabilities.
   *
   * @param id PipeWire global identifier.
   * @param capabilities Known or unknown capability state.
   * @return True when the tracked capability state changed.
   */
  bool updateSampleRateCapabilities(
      std::uint32_t id, SampleRateCapabilities capabilities);

  /**
   * Replaces one device's active physical Format rate.
   *
   * @param id PipeWire global identifier.
   * @param sampleRate Active rate in hertz, or zero while idle.
   * @return True when the tracked active rate changed.
   */
  bool updateActiveSampleRate(std::uint32_t id,
                              std::uint32_t sampleRate);

  /**
   * Updates default.audio.sink from PipeWire metadata.
   *
   * Empty and PipeTune's own node name are ignored so that making PipeTune the
   * effective default does not overwrite the last physical fallback.
   *
   * @param nodeName Physical default node.name.
   * @return True when selectedTarget() changed.
   */
  bool setDefaultTarget(std::string nodeName);

  /**
   * Replaces the user-preferred output.
   *
   * An unavailable name remains configured while a physical fallback is used.
   *
   * @param nodeName Preferred node.name, or empty to clear the preference.
   * @return True when the preference, selected output, or reason changed.
   */
  bool setPreferredTarget(std::string nodeName);

  /**
   * Clears the user preference and selects the physical system default.
   *
   * @return True when the preference, selected output, or reason changed.
   */
  bool clearPreferredTarget();

  /**
   * Marks initial registry enumeration complete.
   *
   * Before this call fallback selection follows every newly discovered
   * priority improvement. Afterwards a usable current fallback is retained
   * until the session default changes or the device disappears.
   */
  void commitSelection() noexcept;

  /** Returns the selected physical node.name, or empty when none is usable. */
  std::string_view selectedTarget() const noexcept;
  /** Returns the configured preferred node.name, or empty when automatic. */
  std::string_view preferredTarget() const noexcept;
  /** Returns the remembered physical system-default node.name. */
  std::string_view systemDefaultTarget() const noexcept;
  /** Returns true when a user preference is configured. */
  bool hasPreferredTarget() const noexcept;
  /** Returns why the current output was selected. */
  OutputSelectionReason selectionReason() const noexcept;
  /** Returns a copy of the selected output's rate capabilities. */
  SampleRateCapabilities selectedSampleRateCapabilities() const;
  /** Returns the selected output's active physical rate, or zero. */
  std::uint32_t selectedActiveSampleRate() const noexcept;

  /**
   * Returns eligible devices sorted by description and node.name.
   *
   * @return Presentation-ready copy of the current physical outputs.
   */
  std::vector<OutputDevice> availableDevices() const;

private:
  bool recomputeSelection();
  bool isEligible(const OutputDevice &device) const noexcept;
  const OutputDevice *findEligibleByName(std::string_view name) const noexcept;

  std::string excludedNodeName_;
  std::string preferredTarget_;
  std::string defaultTarget_;
  std::string selectedTarget_;
  OutputSelectionReason selectionReason_;
  std::unordered_map<std::uint32_t, OutputDevice> devices_;
  bool selectionCommitted_;
};

} // namespace pipetune

#endif
