/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output-mapping-model.h"

#include <algorithm>
#include <utility>

namespace pipetune_gtk {

std::vector<std::size_t> configuredOutputOrder(
    const pipetune::OutputConfiguration &configuration) {
  auto firstChannel = std::vector<std::size_t>(
      configuration.outputs.size(), configuration.channels.size());
  for (auto slot = std::size_t{0}; slot < configuration.channels.size(); ++slot) {
    for (auto output = std::size_t{0}; output < configuration.outputs.size(); ++output) {
      if (configuration.channels[slot].outputId == configuration.outputs[output].id &&
          firstChannel[output] == configuration.channels.size()) {
        firstChannel[output] = slot;
        break;
      }
    }
  }
  auto order = std::vector<std::size_t>{};
  order.reserve(configuration.outputs.size());
  for (auto output = std::size_t{0}; output < configuration.outputs.size(); ++output)
    order.push_back(output);
  std::stable_sort(order.begin(), order.end(), [&](const auto left, const auto right) {
    return firstChannel[left] < firstChannel[right];
  });
  return order;
}

pipetune::OutputConfigurationResult removeOutputDevice(
    const pipetune::OutputConfiguration &configuration,
    std::string_view outputId) {
  auto candidate = configuration;
  const auto found = std::find_if(candidate.outputs.begin(), candidate.outputs.end(),
      [outputId](const auto &output) { return output.id == outputId; });
  if (found == candidate.outputs.end())
    return {configuration, "saved output is unavailable for removal"};
  candidate.outputs.erase(found);
  for (auto &slot : candidate.channels) {
    if (slot.outputId != outputId) continue;
    slot.outputId.clear();
    slot.deviceChannel = 0;
  }
  while (!candidate.channels.empty() &&
         configuration.channels[candidate.channels.size() - 1].outputId == outputId)
    candidate.channels.pop_back();
  // A draft may temporarily have no enabled output while the user selects a
  // replacement. All other mapping invariants must still hold.
  auto validation = candidate;
  validation.mode = pipetune::OutputMode::single;
  const auto error = pipetune::validateOutputConfiguration(validation);
  if (!error.empty()) return {configuration, error};
  return {std::move(candidate), {}};
}

pipetune::OutputConfigurationResult compactOutputChannels(
    const pipetune::OutputConfiguration &configuration) {
  auto candidate = configuration;
  std::erase_if(candidate.channels, [](const auto &slot) {
    return slot.outputId.empty();
  });
  auto validation = candidate;
  validation.mode = pipetune::OutputMode::single;
  const auto error = pipetune::validateOutputConfiguration(validation);
  if (!error.empty()) return {configuration, error};
  return {std::move(candidate), {}};
}

pipetune::OutputConfigurationResult moveOutputChannel(
    const pipetune::OutputConfiguration &configuration,
    std::size_t from, std::size_t to) {
  if (from >= configuration.channels.size() || to >= configuration.channels.size()) {
    return {configuration, "channel move is outside the saved channel range"};
  }
  auto candidate = configuration;
  auto slot = std::move(candidate.channels[from]);
  candidate.channels.erase(candidate.channels.begin() + static_cast<std::ptrdiff_t>(from));
  candidate.channels.insert(candidate.channels.begin() + static_cast<std::ptrdiff_t>(to), std::move(slot));
  // Enablement is checked when confirming the complete draft.
  auto validation = candidate;
  validation.mode = pipetune::OutputMode::single;
  const auto error = pipetune::validateOutputConfiguration(validation);
  if (!error.empty()) return {configuration, error};
  return {std::move(candidate), {}};
}

pipetune::OutputConfigurationResult replaceOutputDevice(
    const pipetune::OutputConfiguration &configuration,
    std::string_view outputId, const pipetune::OutputDeviceDescription &device) {
  auto candidate = configuration;
  const auto found = std::find_if(candidate.outputs.begin(), candidate.outputs.end(),
      [outputId](const auto &output) { return output.id == outputId; });
  if (found == candidate.outputs.end()) return {configuration, "saved output is unavailable for reassignment"};
  const auto width = device.channelPositions.size();
  if (width == 0 || width > 16) return {configuration, "replacement requires one through sixteen output channels"};
  const auto previousWidth = found->device.channelPositions.size();
  found->device = device;
  for (auto &slot : candidate.channels) {
    if (slot.outputId == outputId && slot.deviceChannel >= width) {
      // Reserve the final DSP number and its user's label rather than shifting
      // another device into the removed physical channel's place.
      slot.outputId.clear();
      slot.deviceChannel = 0;
    }
  }
  for (auto channel = previousWidth; channel < width; ++channel) {
    candidate.channels.push_back({found->id, static_cast<std::uint32_t>(channel), {}});
  }
  // Replacement preserves enablement, including a temporarily disabled draft.
  auto validation = candidate;
  validation.mode = pipetune::OutputMode::single;
  const auto error = pipetune::validateOutputConfiguration(validation);
  if (!error.empty()) return {configuration, error};
  return {std::move(candidate), {}};
}

} // namespace pipetune_gtk
