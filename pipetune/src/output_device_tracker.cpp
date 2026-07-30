#include "output_device_tracker.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune {

OutputDeviceTracker::OutputDeviceTracker(std::string excludedNodeName,
                                         std::string preferredTarget)
    : excludedNodeName_(std::move(excludedNodeName)),
      preferredTarget_(std::move(preferredTarget)), defaultTarget_(),
      selectedTarget_(),
      selectionReason_(OutputSelectionReason::unavailable), devices_(),
      selectionCommitted_(false) {}

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

bool OutputDeviceTracker::updateSampleRateCapabilities(
    std::uint32_t id, SampleRateCapabilities capabilities) {
  const auto found = devices_.find(id);
  if (found == devices_.end() ||
      !normalizeSampleRateCapabilities(capabilities) ||
      found->second.sampleRateCapabilities == capabilities) {
    return false;
  }
  found->second.sampleRateCapabilities = std::move(capabilities);
  return true;
}

bool OutputDeviceTracker::updateActiveSampleRate(
    std::uint32_t id, std::uint32_t sampleRate) {
  const auto found = devices_.find(id);
  if (found == devices_.end() ||
      found->second.activeSampleRate == sampleRate) {
    return false;
  }
  found->second.activeSampleRate = sampleRate;
  return true;
}

bool OutputDeviceTracker::updateVolumeControlAvailability(
    std::uint32_t id, bool available) {
  const auto found = devices_.find(id);
  if (found == devices_.end() ||
      found->second.volumeControlAvailable == available) {
    return false;
  }
  found->second.volumeControlAvailable = available;
  static_cast<void>(recomputeSelection());
  return true;
}

bool OutputDeviceTracker::setDefaultTarget(std::string nodeName) {
  if (nodeName.empty() || nodeName == excludedNodeName_) {
    return false;
  }
  if (nodeName == defaultTarget_) {
    return false;
  }
  defaultTarget_ = std::move(nodeName);
  return recomputeSelection();
}

bool OutputDeviceTracker::setPreferredTarget(std::string nodeName) {
  if (nodeName == preferredTarget_) {
    return false;
  }
  preferredTarget_ = std::move(nodeName);
  static_cast<void>(recomputeSelection());
  return true;
}

bool OutputDeviceTracker::clearPreferredTarget() {
  if (preferredTarget_.empty()) {
    return false;
  }
  preferredTarget_.clear();
  static_cast<void>(recomputeSelection());
  return true;
}

void OutputDeviceTracker::commitSelection() noexcept {
  selectionCommitted_ = true;
}

std::string_view OutputDeviceTracker::selectedTarget() const noexcept {
  return selectedTarget_;
}

std::string_view OutputDeviceTracker::preferredTarget() const noexcept {
  return preferredTarget_;
}

std::string_view OutputDeviceTracker::systemDefaultTarget() const noexcept {
  return defaultTarget_;
}

bool OutputDeviceTracker::hasPreferredTarget() const noexcept {
  return !preferredTarget_.empty();
}

OutputSelectionReason
OutputDeviceTracker::selectionReason() const noexcept {
  return selectionReason_;
}

SampleRateCapabilities
OutputDeviceTracker::selectedSampleRateCapabilities() const {
  const auto *selected = findEligibleByName(selectedTarget_);
  return selected == nullptr ? SampleRateCapabilities{}
                             : selected->sampleRateCapabilities;
}

std::uint32_t
OutputDeviceTracker::selectedActiveSampleRate() const noexcept {
  const auto *selected = findEligibleByName(selectedTarget_);
  return selected == nullptr ? 0 : selected->activeSampleRate;
}

bool OutputDeviceTracker::isEligible(const OutputDevice &device) const noexcept {
  return !device.virtualNode && !device.name.empty() &&
         device.name != excludedNodeName_ &&
         device.volumeControlAvailable;
}

const OutputDevice *
OutputDeviceTracker::findEligibleByName(std::string_view name) const noexcept {
  if (name.empty()) {
    return nullptr;
  }
  for (const auto &[id, device] : devices_) {
    static_cast<void>(id);
    if (isEligible(device) && device.name == name) {
      return &device;
    }
  }
  return nullptr;
}

std::vector<OutputDevice> OutputDeviceTracker::availableDevices() const {
  auto available = std::vector<OutputDevice>{};
  available.reserve(devices_.size());
  for (const auto &[id, device] : devices_) {
    static_cast<void>(id);
    if (isEligible(device)) {
      available.push_back(device);
    }
  }
  std::sort(available.begin(), available.end(),
            [](const OutputDevice &left, const OutputDevice &right) {
              if (left.description != right.description) {
                return left.description < right.description;
              }
              return left.name < right.name;
            });
  return available;
}

bool OutputDeviceTracker::recomputeSelection() {
  auto nextTarget = std::string{};
  auto nextReason = OutputSelectionReason::unavailable;
  const auto *preferred = findEligibleByName(preferredTarget_);
  if (preferred != nullptr) {
    nextTarget = preferred->name;
    nextReason = OutputSelectionReason::preferred;
  } else {
    const auto *systemDefault = findEligibleByName(defaultTarget_);
    if (systemDefault != nullptr) {
      nextTarget = systemDefault->name;
    } else if (selectionCommitted_) {
      const auto *retained = findEligibleByName(selectedTarget_);
      if (retained != nullptr) {
        nextTarget = retained->name;
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
    if (!nextTarget.empty()) {
      nextReason = preferredTarget_.empty()
                       ? OutputSelectionReason::systemDefault
                       : OutputSelectionReason::fallback;
    }
  }

  if (nextTarget == selectedTarget_ && nextReason == selectionReason_) {
    return false;
  }
  selectedTarget_ = std::move(nextTarget);
  selectionReason_ = nextReason;
  return true;
}

} // namespace pipetune
