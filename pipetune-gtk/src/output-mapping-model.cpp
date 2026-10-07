/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output-mapping-model.h"

#include <algorithm>
#include <utility>

namespace pipetune_gtk {

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
  const auto error = pipetune::validateOutputConfiguration(candidate);
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
  const auto error = pipetune::validateOutputConfiguration(candidate);
  if (!error.empty()) return {configuration, error};
  return {std::move(candidate), {}};
}

} // namespace pipetune_gtk
