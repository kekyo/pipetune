#include "output_device_tracker.h"

#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace pipetune {

OutputDeviceTracker::OutputDeviceTracker(std::string excludedNodeName,
                                         std::string requestedTarget)
    : excludedNodeName_(std::move(excludedNodeName)),
      requestedTarget_(std::move(requestedTarget)), defaultTarget_(),
      selectedTarget_(), devices_(), selectionCommitted_(false) {}

bool OutputDeviceTracker::updateDevice(OutputDevice device) {
  devices_.insert_or_assign(device.id, std::move(device));
  return recomputeSelection();
}

bool OutputDeviceTracker::removeDevice(std::uint32_t id) {
  if (devices_.erase(id) == 0) {
    return false;
  }
  return recomputeSelection();
}

bool OutputDeviceTracker::setDefaultTarget(std::string nodeName) {
  defaultTarget_ = std::move(nodeName);
  return recomputeSelection();
}

void OutputDeviceTracker::commitSelection() noexcept {
  selectionCommitted_ = true;
}

std::string_view OutputDeviceTracker::selectedTarget() const noexcept {
  return selectedTarget_;
}

bool OutputDeviceTracker::hasExplicitTarget() const noexcept {
  return !requestedTarget_.empty();
}

bool OutputDeviceTracker::isEligible(const OutputDevice &device) const noexcept {
  return !device.virtualNode && !device.name.empty() &&
         device.name != excludedNodeName_;
}

bool OutputDeviceTracker::recomputeSelection() {
  auto nextTarget = std::string{};
  if (!requestedTarget_.empty()) {
    auto selectedId = std::numeric_limits<std::uint32_t>::max();
    for (const auto &[id, device] : devices_) {
      if (isEligible(device) &&
          (device.name == requestedTarget_ ||
           device.objectSerial == requestedTarget_) &&
          id < selectedId) {
        selectedId = id;
        nextTarget = device.name;
      }
    }
  } else {
    for (const auto &[id, device] : devices_) {
      static_cast<void>(id);
      if (isEligible(device) && device.name == defaultTarget_) {
        nextTarget = device.name;
        break;
      }
    }

    if (selectionCommitted_ && nextTarget.empty() &&
        !selectedTarget_.empty()) {
      for (const auto &[id, device] : devices_) {
        static_cast<void>(id);
        if (isEligible(device) && device.name == selectedTarget_) {
          nextTarget = selectedTarget_;
          break;
        }
      }
    }

    if (nextTarget.empty()) {
      const OutputDevice *bestDevice = nullptr;
      for (const auto &[id, device] : devices_) {
        static_cast<void>(id);
        if (!isEligible(device)) {
          continue;
        }
        if (bestDevice == nullptr || device.priority > bestDevice->priority ||
            (device.priority == bestDevice->priority &&
             device.id < bestDevice->id)) {
          bestDevice = &device;
        }
      }
      if (bestDevice != nullptr) {
        nextTarget = bestDevice->name;
      }
    }
  }

  if (nextTarget == selectedTarget_) {
    return false;
  }
  selectedTarget_ = std::move(nextTarget);
  return true;
}

} // namespace pipetune
