/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output_command.h"

#include "pipetune/control_socket.h"
#include "pipetune/output_inventory.h"
#include "pipetune/startup_config.h"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>
#include <utility>

namespace pipetune {

OutputStatusQueryResult queryOutputStatus(const std::filesystem::path &socketPath) {
  const auto exchange = exchangeControlMessage(socketPath, makeStatusControlRequest());
  if (!exchange.error.empty()) {
    return {.status = {}, .json = {}, .error = exchange.error,
            .unavailable = exchange.unavailable};
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid || !response.success) {
    return {.status = {}, .json = {}, .error = std::move(response.error)};
  }
  if (response.kind != ControlResponseKind::response) {
    return {.status = {}, .json = {}, .error = "daemon returned an unexpected status event"};
  }
  return {.status = std::move(response.status), .json = exchange.response, .error = {}};
}

OutputConfigurationResult selectOutputDevices(const OutputConfiguration &previous,
    std::span<const AvailableOutput> inventory, std::span<const std::string> nodeNames) {
  const auto previousError = validateOutputConfiguration(previous);
  if (!previousError.empty()) return {previous, previousError};
  if (nodeNames.empty()) return {previous, "select at least one output"};
  auto candidate = previous;
  // Intermediate selections may contain no enabled output. Validate the
  // complete multiple-mode candidate after preserving and enabling slots.
  candidate.mode = OutputMode::single;
  for (auto &output : candidate.outputs) output.enabled = false;
  auto names = std::set<std::string>{};
  for (const auto &name : nodeNames) {
    if (!names.insert(name).second) return {previous, "duplicate output node: " + name};
    const auto match = [&name](const auto &output) { return output.nodeName == name; };
    if (std::count_if(inventory.begin(), inventory.end(), match) != 1)
      return {previous, "output node is missing or ambiguous: " + name};
    const auto &device = std::find_if(inventory.begin(), inventory.end(), match)->device;
    const auto sameIdentity = [&device](const auto &output) { return output.device.identity == device.identity; };
    const auto saved = std::find_if(candidate.outputs.begin(), candidate.outputs.end(),
        [&device, &sameIdentity](const auto &output) {
          return sameIdentity(output) && output.device.profile == device.profile &&
                 output.device.channelPositions == device.channelPositions;
        });
    auto id = std::string{};
    if (saved != candidate.outputs.end()) id = saved->id;
    else {
      if (std::any_of(candidate.outputs.begin(), candidate.outputs.end(), sameIdentity))
        return {previous, "changed output profile or layout requires explicit reassignment: " + name};
      for (auto suffix = std::size_t{1}; id.empty(); ++suffix) {
        const auto proposed = "output-" + std::to_string(suffix);
        if (std::none_of(candidate.outputs.begin(), candidate.outputs.end(),
                        [&proposed](const auto &output) { return output.id == proposed; })) id = proposed;
      }
    }
    auto appended = appendConfiguredOutput(candidate, {id, true, device});
    if (!appended.error.empty()) return {previous, std::move(appended.error)};
    candidate = std::move(appended.configuration);
  }
  candidate.mode = OutputMode::multiple;
  const auto error = validateOutputConfiguration(candidate);
  if (!error.empty()) return {previous, error};
  for (const auto &resolved : resolveConfiguredOutputs(candidate, inventory)) {
    if (resolved.state != OutputConnectionState::connected && resolved.state != OutputConnectionState::disabled)
      return {previous, "selected output identity or profile cannot be resolved uniquely: " + resolved.outputId};
  }
  return {std::move(candidate), {}};
}

static OutputStatusQueryResult applyOutput(const std::filesystem::path &socketPath,
    const OutputConfiguration &configuration, std::uint64_t expectedRevision) {
  const auto request = makeSetOutputControlRequest(configuration, expectedRevision);
  if (request.empty()) return {.status = {}, .json = {}, .error = "cannot encode output configuration"};
  const auto exchange = exchangeControlMessage(socketPath, request);
  if (!exchange.error.empty()) {
    return {.status = {}, .json = {},
            .error = "could not confirm live output configuration: " + exchange.error};
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid)
    return {.status = {}, .json = {}, .error = "could not confirm live output configuration: " + response.error};
  if (!response.success) return {.status = {}, .json = {}, .error = std::move(response.error)};
  if (response.kind != ControlResponseKind::response || response.status.outputConfiguration != configuration)
    return {.status = {}, .json = {}, .error = "daemon did not confirm the requested output configuration"};
  return {.status = std::move(response.status), .json = exchange.response, .error = {}};
}

PersistentOutputResult executeOutputChange(const PersistentOutputOptions &options, const OutputChange &change) {
  auto saved = loadStartupConfig(options.configPath);
  if (!saved.error.empty()) return {.notice = {}, .error = std::move(saved.error)};
  const auto previous = queryOutputStatus(options.socketPath);
  if (!previous.error.empty() && !previous.unavailable) return {.notice = {}, .error = previous.error};
  auto candidate = previous.unavailable ? saved.config.outputConfiguration : previous.status.outputConfiguration;
  switch (change.kind) {
  case OutputChangeKind::replace:
    candidate = change.configuration;
    break;
  case OutputChangeKind::mode:
    candidate.mode = change.configuration.mode;
    break;
  case OutputChangeKind::select: {
    auto inventory = OutputInventoryResult{};
    if (previous.unavailable) inventory = queryAvailableOutputs();
    else if (!previous.status.outputInventoryError.empty()) inventory.error = previous.status.outputInventoryError;
    else if (!previous.status.outputInventoryReady) inventory.error = "output inventory is not ready; retry after enumeration";
    else inventory.outputs = previous.status.availableOutputs;
    if (!inventory.error.empty()) return {.notice = {}, .error = std::move(inventory.error)};
    auto selected = selectOutputDevices(candidate, inventory.outputs, change.nodes);
    if (!selected.error.empty()) return {.notice = {}, .error = std::move(selected.error)};
    candidate = std::move(selected.configuration);
    break;
  }
  }
  const auto validation = validateOutputConfiguration(candidate);
  if (!validation.empty()) return {.notice = {}, .error = validation};
  auto result = PersistentOutputResult{};
  if (previous.unavailable) {
    result.notice = "daemon is unavailable; output configuration will apply at the next start";
  } else {
    auto applied = applyOutput(options.socketPath, candidate, previous.status.configurationRevision);
    if (!applied.error.empty()) return {.notice = {}, .error = applied.error + "; output settings were not saved"};
    result.status = std::move(applied.status);
    result.liveApplied = true;
  }
  saved.config.outputConfiguration = candidate;
  const auto persistenceError = saveStartupConfig(options.configPath, saved.config);
  if (!persistenceError.empty()) {
    result.error = "cannot save output configuration: " + persistenceError;
    if (result.liveApplied) {
      // The revision condition also covers restoration: never overwrite a
      // different client's intervening edit to repair this failed save.
      auto restored = applyOutput(options.socketPath, previous.status.outputConfiguration,
                                  result.status.configurationRevision);
      result.liveApplied = false;
      if (restored.error.empty()) {
        result.rollbackApplied = true;
        result.status = std::move(restored.status);
        result.error += "; previous live output configuration was restored";
      } else {
        result.error += "; could not restore previous live output configuration: " + restored.error +
                        "; inspect output get before making further changes";
      }
    }
    return result;
  }
  result.success = true;
  result.persistenceApplied = true;
  return result;
}

static std::string tableCell(std::string_view value) {
  auto result = std::string{};
  for (const auto character : value) {
    if (character == '\n') result += "\\n";
    else if (character == '\r') result += "\\r";
    else if (character == '\t') result += "\\t";
    else if (character == '|') result += "\\|";
    else if (static_cast<unsigned char>(character) < 32 || character == 127) result += '?';
    else result += character;
  }
  return result;
}

static std::string_view connectionName(OutputConnectionState state) {
  switch (state) {
  case OutputConnectionState::connected: return "connected";
  case OutputConnectionState::disabled: return "disabled";
  case OutputConnectionState::missing: return "missing";
  case OutputConnectionState::profileMismatch: return "profile mismatch";
  case OutputConnectionState::ambiguous: return "ambiguous identity";
  }
  return "unknown";
}

static std::string deviceGainText(std::optional<float> gain) {
  if (!gain) return "unknown";
  if (*gain == 0) return "-inf dB";
  auto text = std::ostringstream{};
  text << std::fixed << std::setprecision(1) << 20.0 * std::log10(*gain) << " dB";
  return text.str();
}

std::string formatOutputStatus(const ControlRuntimeStatus &status) {
  const auto &configuration = status.outputConfiguration;
  const auto single = configuration.mode == OutputMode::single;
  auto text = std::ostringstream{};
  text << "Output mode: " << (single ? "single (OS selects the physical output)" : "multiple") << '\n'
       << "DSP channels: " << outputDspChannelCount(configuration) << '\n';
  if (!status.outputInventoryError.empty()) text << "Device inventory error: " << tableCell(status.outputInventoryError) << '\n';
  else if (!status.outputInventoryReady) text << "Device inventory: pending\n";
  if (single && !configuration.channels.empty()) text << "Saved multiple-output assignments are inactive in single mode.\n";
  text << "Device presence does not confirm audio transport or delay compensation.\n"
       << "EffeTune output | Device | Device channel | Profile | Presence | Label\n";
  const auto resolved = resolveConfiguredOutputs(configuration, status.availableOutputs);
  for (auto index = std::size_t{0}; index < configuration.channels.size(); ++index) {
    const auto &slot = configuration.channels[index];
    text << "Ch " << index + 1 << " | ";
    const auto output = std::find_if(configuration.outputs.begin(), configuration.outputs.end(),
        [&slot](const auto &item) { return item.id == slot.outputId; });
    if (output == configuration.outputs.end()) text << "Unassigned | - | - | reserved";
    else {
      const auto outputIndex = static_cast<std::size_t>(output - configuration.outputs.begin());
      const auto presence = !output->enabled ? std::string_view("disabled") :
          !status.outputInventoryError.empty() ? std::string_view("inventory error") :
          !status.outputInventoryReady ? std::string_view("inventory pending") : connectionName(resolved[outputIndex].state);
      text << tableCell(output->device.name) << " | " << slot.deviceChannel + 1 << " ("
           << tableCell(output->device.channelPositions[slot.deviceChannel]) << ") | "
           << tableCell(output->device.profile) << " | " << presence;
    }
    text << " | " << tableCell(slot.label) << '\n';
  }
  if (configuration.channels.empty()) text << "No saved multiple-output assignments.\n";
  if (!status.availableOutputs.empty()) {
    text << "Physical output controls (reported; separate from master volume):\n";
    for (const auto &output : status.availableOutputs) {
      const auto found = std::find_if(status.outputVolumes.begin(), status.outputVolumes.end(),
          [&output](const auto &entry) { return entry.nodeSerial == output.nodeSerial; });
      const auto &volume = found == status.outputVolumes.end() ? OutputVolumeState{} : *found;
      text << tableCell(output.device.name) << " | " << (volume.muted ? (*volume.muted ? "muted" : "unmuted") : "mute unknown")
           << " | scalar gain: " << deviceGainText(volume.volume) << " | channel gains: ";
      if (volume.channelVolumes.empty()) text << "unknown";
      for (auto index = std::size_t{0}; index < volume.channelVolumes.size(); ++index) {
        if (index != 0) text << ", ";
        text << deviceGainText(volume.channelVolumes[index]);
      }
      text << '\n';
    }
  }
  return text.str();
}

} // namespace pipetune
