#ifndef PIPETUNE_OUTPUT_DEVICE_TRACKER_H
#define PIPETUNE_OUTPUT_DEVICE_TRACKER_H

#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace pipetune {

/**
 * Describes one PipeWire Audio/Sink candidate.
 */
struct OutputDevice {
  /** PipeWire global identifier. */
  std::uint32_t id;
  /** Stable node.name used as target.object. */
  std::string name;
  /** Decimal object.serial value, when available. */
  std::string objectSerial;
  /** Session-manager preference; larger values are preferred. */
  std::int32_t priority;
  /** True for software or other virtual sinks. */
  bool virtualNode;
};

/**
 * Selects a non-PipeTune physical output as registry state changes.
 */
class OutputDeviceTracker final {
public:
  /**
   * Creates an empty tracker.
   *
   * @param excludedNodeName PipeTune virtual sink name.
   * @param requestedTarget Explicit node name/object serial, or empty for
   * automatic default-device tracking.
   */
  OutputDeviceTracker(std::string excludedNodeName, std::string requestedTarget);

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
   * Updates default.audio.sink from PipeWire metadata.
   *
   * @param nodeName Default node name, or empty when unset.
   * @return True when selectedTarget() changed.
   */
  bool setDefaultTarget(std::string nodeName);

  /** Returns the selected physical node.name, or empty when none is usable. */
  std::string_view selectedTarget() const noexcept;
  /** Returns true when an explicit target was requested. */
  bool hasExplicitTarget() const noexcept;

private:
  bool recomputeSelection();
  bool isEligible(const OutputDevice &device) const noexcept;

  std::string excludedNodeName_;
  std::string requestedTarget_;
  std::string defaultTarget_;
  std::string selectedTarget_;
  std::unordered_map<std::uint32_t, OutputDevice> devices_;
};

} // namespace pipetune

#endif
