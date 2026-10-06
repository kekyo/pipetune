/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/output_configuration.h"

#include <algorithm>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static pipetune::ConfiguredOutput stereo(std::string id, std::string location) {
  return {.id = std::move(id), .enabled = true,
          .device = {.identity = {.api = "alsa", .location = std::move(location),
              .port = "pcm:0:0", .vendor = "vendor", .product = "product", .serial = "model"},
              .name = "Stereo output", .profile = "analog-stereo", .channelPositions = {"FL", "FR"}}};
}

static pipetune::OutputConfiguration twoOutputs() {
  return {.mode = pipetune::OutputMode::multiple,
          .outputs = {stereo("a", "usb:1"), stereo("b", "usb:2")},
          .channels = {{"a", 0, "Main left"}, {"a", 1, "Main right"}, {"b", 0, ""}, {"b", 1, ""}}};
}

static bool testValidation() {
  const auto valid = twoOutputs();
  if (!check(pipetune::validateOutputConfiguration({}).empty(), "default single mode must be valid") ||
      !check(pipetune::validateOutputConfiguration(valid).empty(), "two complete stereo assignments must be valid")) return false;
  auto bad = valid;
  bad.channels[3].deviceChannel = 0;
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "duplicate output channels must be rejected")) return false;
  bad = valid;
  bad.channels[0].deviceChannel = 2;
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "an out-of-range physical channel must be rejected")) return false;
  bad = valid;
  bad.channels.pop_back();
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "all channels of each selected profile must be assigned")) return false;
  bad = valid;
  bad.channels[0].outputId = "unknown";
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "unknown output references must be rejected")) return false;
  bad = valid;
  bad.outputs[1].id = "a";
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "duplicate saved output identifiers must be rejected")) return false;
  bad = valid;
  bad.outputs[1].device.identity = bad.outputs[0].device.identity;
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "the same output cannot be selected twice")) return false;
  bad = valid;
  bad.outputs[0].device.identity.location.clear();
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "an output without stable identity must be rejected")) return false;
  bad = valid;
  for (auto &output : bad.outputs) output.enabled = false;
  if (!check(!pipetune::validateOutputConfiguration(bad).empty(), "multiple mode needs an enabled output")) return false;
  bad.mode = pipetune::OutputMode::single;
  if (!check(pipetune::validateOutputConfiguration(bad).empty(), "single mode may retain disabled multi-output choices")) return false;
  bad.channels.resize(17);
  return check(!pipetune::validateOutputConfiguration(bad).empty(), "reserved slots count toward the sixteen-channel limit");
}

static bool testAppendingAndReservations() {
  auto original = twoOutputs();
  original.outputs[0].enabled = false;
  const auto added = pipetune::appendConfiguredOutput(original, stereo("c", "usb:3"));
  if (!check(added.error.empty() && added.configuration.channels.size() == 6,
             "new devices must append every channel") ||
      !check(std::equal(original.channels.begin(), original.channels.end(), added.configuration.channels.begin()),
             "disabled and connected outputs must keep their slots") ||
      !check(added.configuration.channels[4].outputId == "c" && added.configuration.channels[4].deviceChannel == 0,
             "new device left must become Ch 5")) return false;
  const auto enabled = pipetune::appendConfiguredOutput(added.configuration, stereo("a", "usb:1"));
  if (!check(enabled.error.empty() && enabled.configuration.outputs[0].enabled &&
             enabled.configuration.channels == added.configuration.channels,
             "re-enabling an output must recover its exact slots")) return false;
  auto full = original;
  full.channels.resize(16);
  const auto rejected = pipetune::appendConfiguredOutput(full, stereo("c", "usb:3"));
  if (!check(!rejected.error.empty() && rejected.configuration == full,
             "exceeding the limit must preserve the complete previous configuration")) return false;
  auto changed = stereo("a", "usb:1");
  changed.device.channelPositions = {"AUX0", "AUX1"};
  const auto mismatch = pipetune::appendConfiguredOutput(original, changed);
  return check(!mismatch.error.empty() && mismatch.configuration == original,
               "a changed layout requires explicit reassignment");
}

static bool testResolution() {
  const auto saved = twoOutputs();
  auto inventory = std::vector<pipetune::AvailableOutput>{
      {saved.outputs[1].device, "runtime-b", 800}, {saved.outputs[0].device, "runtime-a", 900}};
  auto resolved = pipetune::resolveConfiguredOutputs(saved, inventory);
  if (!check(resolved.size() == 2 && resolved[0].inventoryIndex == 1 && resolved[1].inventoryIndex == 0,
             "enumeration order must not reorder saved channels")) return false;
  inventory.pop_back();
  resolved = pipetune::resolveConfiguredOutputs(saved, inventory);
  if (!check(resolved[0].state == pipetune::OutputConnectionState::missing && !resolved[0].inventoryIndex &&
             resolved[1].state == pipetune::OutputConnectionState::connected,
             "a disconnected output must not fall back to the surviving device")) return false;
  inventory.push_back({saved.outputs[0].device, "reconnected-a", 901});
  resolved = pipetune::resolveConfiguredOutputs(saved, inventory);
  if (!check(resolved[0].state == pipetune::OutputConnectionState::connected && resolved[0].inventoryIndex == 1,
             "reconnection must use stable identity even when runtime IDs and names change")) return false;
  inventory[1].device.channelPositions = {"FR", "FL"};
  resolved = pipetune::resolveConfiguredOutputs(saved, inventory);
  if (!check(resolved[0].state == pipetune::OutputConnectionState::profileMismatch && !resolved[0].inventoryIndex,
             "changed channel order must be reported without routing")) return false;
  inventory[1].device = saved.outputs[0].device;
  inventory.push_back({saved.outputs[0].device, "duplicate-a", 902});
  resolved = pipetune::resolveConfiguredOutputs(saved, inventory);
  if (!check(resolved[0].state == pipetune::OutputConnectionState::ambiguous && !resolved[0].inventoryIndex,
             "ambiguous identity must never select the first enumerated node")) return false;
  auto disabled = saved;
  disabled.outputs[0].enabled = false;
  resolved = pipetune::resolveConfiguredOutputs(disabled, inventory);
  return check(resolved[0].state == pipetune::OutputConnectionState::disabled && !resolved[0].inventoryIndex,
               "disabled slots must never reconnect automatically");
}

static bool testDspWidth() {
  auto configuration = twoOutputs();
  if (!check(pipetune::outputDspChannelCount(configuration) == 4, "DSP must cover all fixed slots")) return false;
  configuration.channels.resize(16);
  if (!check(pipetune::validateOutputConfiguration(configuration).empty() &&
             pipetune::outputDspChannelCount(configuration) == 16, "unassigned slots must retain DSP width up to sixteen")) return false;
  configuration.mode = pipetune::OutputMode::single;
  if (!check(pipetune::outputDspChannelCount(configuration) == 2, "single mode must retain its stereo width")) return false;
  configuration = {.mode = pipetune::OutputMode::multiple, .outputs = {stereo("mono", "usb:3")}, .channels = {{"mono", 0, ""}}};
  configuration.outputs[0].device.channelPositions = {"MONO"};
  return check(pipetune::validateOutputConfiguration(configuration).empty() &&
               pipetune::outputDspChannelCount(configuration) == 2,
               "a mono output must not shrink the stereo DSP input");
}

int main() {
  auto success = testValidation();
  success = testAppendingAndReservations() && success;
  success = testResolution() && success;
  success = testDspWidth() && success;
  return success ? 0 : 1;
}
