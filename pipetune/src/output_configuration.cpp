/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/output_configuration.h"

#include <algorithm>
#include <map>
#include <utility>

namespace pipetune {

std::string validateOutputConfiguration(const OutputConfiguration &configuration) {
  if (configuration.mode != OutputMode::single && configuration.mode != OutputMode::multiple)
    return "output mode must be single or multiple";
  if (configuration.channels.size() > 16)
    return "output configuration exceeds sixteen channels, including reserved slots";
  auto assigned = std::map<std::string, std::vector<bool>>{};
  auto enabled = false;
  for (auto index = std::size_t{0}; index < configuration.outputs.size(); ++index) {
    const auto &output = configuration.outputs[index];
    const auto &device = output.device;
    if (output.id.empty() || assigned.contains(output.id))
      return "configured output identifiers must be nonempty and unique";
    if (device.identity.api.empty() || device.identity.location.empty() || device.identity.port.empty())
      return "configured output requires a stable API, location, and output identifier";
    if (device.name.empty() || device.channelPositions.empty() || device.channelPositions.size() > 16 ||
        std::any_of(device.channelPositions.begin(), device.channelPositions.end(),
            [](const auto &position) { return position.empty(); }))
      return "configured output requires a name and one through sixteen channel positions";
    for (auto previous = std::size_t{0}; previous < index; ++previous) {
      const auto &other = configuration.outputs[previous].device;
      if (device.identity == other.identity && device.profile == other.profile)
        return "the same output profile cannot be configured more than once";
    }
    assigned.emplace(output.id, std::vector<bool>(device.channelPositions.size(), false));
    enabled = enabled || output.enabled;
  }
  for (const auto &slot : configuration.channels) {
    if (slot.outputId.empty()) {
      if (slot.deviceChannel != 0) return "an unassigned slot must not reference a device channel";
      continue;
    }
    const auto found = assigned.find(slot.outputId);
    if (found == assigned.end()) return "channel slot refers to an unknown output";
    if (slot.deviceChannel >= found->second.size()) return "channel slot exceeds the output profile width";
    if (found->second[slot.deviceChannel]) return "each device channel must have exactly one DSP slot";
    found->second[slot.deviceChannel] = true;
  }
  for (const auto &[id, channels] : assigned) {
    if (std::find(channels.begin(), channels.end(), false) != channels.end())
      return "every channel of configured output " + id + " must have a DSP slot";
  }
  if (configuration.mode == OutputMode::multiple && !enabled)
    return "multiple output mode requires at least one enabled output";
  return {};
}

OutputConfigurationResult appendConfiguredOutput(
    const OutputConfiguration &configuration, const ConfiguredOutput &output) {
  auto candidate = configuration;
  const auto found = std::find_if(candidate.outputs.begin(), candidate.outputs.end(),
      [&output](const auto &saved) { return saved.id == output.id; });
  if (found != candidate.outputs.end()) {
    if (found->device.identity != output.device.identity ||
        found->device.profile != output.device.profile ||
        found->device.channelPositions != output.device.channelPositions)
      return {.configuration = configuration, .error = "changed output identity or layout requires explicit reassignment"};
    // Labels and channel slots are independent of live device descriptions.
    found->enabled = output.enabled;
    found->device.name = output.device.name;
  } else {
    candidate.outputs.push_back(output);
    for (auto channel = std::size_t{0}; channel < output.device.channelPositions.size(); ++channel)
      candidate.channels.push_back({output.id, static_cast<std::uint32_t>(channel), {}});
  }
  const auto error = validateOutputConfiguration(candidate);
  if (!error.empty()) return {.configuration = configuration, .error = error};
  return {.configuration = std::move(candidate), .error = {}};
}

std::vector<ResolvedOutput> resolveConfiguredOutputs(
    const OutputConfiguration &configuration, std::span<const AvailableOutput> inventory) {
  auto result = std::vector<ResolvedOutput>{};
  result.reserve(configuration.outputs.size());
  for (const auto &output : configuration.outputs) {
    auto resolved = ResolvedOutput{output.id, OutputConnectionState::missing, std::nullopt};
    if (!output.enabled) {
      resolved.state = OutputConnectionState::disabled;
      result.push_back(std::move(resolved));
      continue;
    }
    auto identities = std::size_t{0};
    auto matches = std::size_t{0};
    for (auto index = std::size_t{0}; index < inventory.size(); ++index) {
      const auto &device = inventory[index].device;
      if (device.identity != output.device.identity) continue;
      ++identities;
      if (device.profile != output.device.profile || device.channelPositions != output.device.channelPositions) continue;
      ++matches;
      resolved.inventoryIndex = index;
    }
    if (matches == 1) resolved.state = OutputConnectionState::connected;
    else {
      resolved.inventoryIndex.reset();
      if (matches > 1) resolved.state = OutputConnectionState::ambiguous;
      else if (identities > 0) resolved.state = OutputConnectionState::profileMismatch;
    }
    result.push_back(std::move(resolved));
  }
  return result;
}

std::uint32_t outputDspChannelCount(const OutputConfiguration &configuration) noexcept {
  return configuration.mode == OutputMode::single ? 2 :
      static_cast<std::uint32_t>(std::max(std::size_t{2}, configuration.channels.size()));
}

} // namespace pipetune
