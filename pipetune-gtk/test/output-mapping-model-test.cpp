/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output-mapping-model.h"

#include <iostream>
#include <string_view>
#include <utility>

static bool check(bool condition, std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static pipetune::OutputConfiguration configuredOutputs() {
  return {
      .mode = pipetune::OutputMode::multiple,
      .outputs = {
          {.id = "a", .enabled = true,
           .device = {.identity = {"node", "dac-a", "dac-a", "", "", ""},
                      .name = "DAC A", .profile = "Stereo", .channelPositions = {"FL", "FR"}}},
          {.id = "b", .enabled = true,
           .device = {.identity = {"node", "dac-b", "dac-b", "", "", ""},
                      .name = "DAC B", .profile = "Stereo", .channelPositions = {"FL", "FR"}}}},
      .channels = {{"a", 0, "Main L"}, {"a", 1, "Main R"},
                   {"b", 0, "Sub L"}, {"b", 1, "Sub R"}, {"", 0, "Reserved"}}};
}

static bool testExplicitChannelMoves() {
  const auto original = configuredOutputs();
  auto expected = original;
  expected.channels = {{"a", 1, "Main R"}, {"b", 0, "Sub L"},
                       {"b", 1, "Sub R"}, {"a", 0, "Main L"}, {"", 0, "Reserved"}};
  const auto moved = pipetune_gtk::moveOutputChannel(original, 0, 3);
  if (!check(moved.error.empty() && moved.configuration == expected,
             "moving a channel must move its purpose label and retain the other slots")) return false;
  const auto restored = pipetune_gtk::moveOutputChannel(moved.configuration, 3, 0);
  if (!check(restored.error.empty() && restored.configuration == original,
             "the inverse move must restore the complete mapping")) return false;
  const auto reserved = pipetune_gtk::moveOutputChannel(original, 4, 0);
  if (!check(reserved.error.empty() && reserved.configuration.channels.front() == original.channels.back() &&
             reserved.configuration.channels[1] == original.channels[0],
             "an explicit move may reposition a reserved slot")) return false;
  const auto invalid = pipetune_gtk::moveOutputChannel(original, 0, 5);
  return check(!invalid.error.empty() && invalid.configuration == original,
               "an out-of-range move must retain the entire configuration");
}

static bool testDeviceReplacement() {
  auto original = configuredOutputs();
  original.outputs[0].enabled = false;
  auto replacement = original.outputs[0].device;
  replacement.identity.location = "new-port";
  replacement.name = "Replacement DAC";
  replacement.profile = "New Stereo";
  replacement.channelPositions = {"AUX0", "AUX1"};
  auto expected = original;
  expected.outputs[0].device = replacement;
  const auto changed = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  if (!check(changed.error.empty() && changed.configuration == expected,
             "replacement must retain enablement, output ID, fixed numbers, and purpose labels")) return false;
  replacement.channelPositions = {"MONO"};
  expected.outputs[0].device = replacement;
  expected.channels[1] = {"", 0, "Main R"};
  const auto narrower = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  if (!check(narrower.error.empty() && narrower.configuration == expected,
             "removed physical channels must become reserved without shifting another device")) return false;
  replacement.channelPositions = {"AUX0", "AUX1", "AUX2", "AUX3"};
  expected = original;
  expected.outputs[0].device = replacement;
  expected.channels.push_back({"a", 2, ""});
  expected.channels.push_back({"a", 3, ""});
  const auto wider = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  return check(wider.error.empty() && wider.configuration == expected,
               "new physical channels must append after every existing reserved slot");
}

static bool testInvalidReplacementsKeepOriginal() {
  auto original = configuredOutputs();
  auto replacement = original.outputs[0].device;
  const auto unknown = pipetune_gtk::replaceOutputDevice(original, "unknown", replacement);
  if (!check(!unknown.error.empty() && unknown.configuration == original,
             "unknown source output must not modify the mapping")) return false;
  const auto duplicate = pipetune_gtk::replaceOutputDevice(original, "a", original.outputs[1].device);
  if (!check(!duplicate.error.empty() && duplicate.configuration == original,
             "a replacement already assigned to another output must fail")) return false;
  for (auto channel = 2u; channel < 13u; ++channel) replacement.channelPositions.push_back("AUX" + std::to_string(channel));
  const auto full = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  if (!check(full.error.empty() && full.configuration.channels.size() == 16 &&
             full.configuration.channels[15] == pipetune::OutputChannelSlot{"a", 12, ""} &&
             full.configuration.channels[2] == original.channels[2],
             "replacement must accept exactly sixteen slots and retain the other device")) return false;
  replacement = original.outputs[0].device;
  original.channels.resize(16);
  replacement.channelPositions.push_back("AUX2");
  const auto tooWide = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  if (!check(!tooWide.error.empty() && tooWide.configuration == original,
             "replacement must include reserved slots in the sixteen-channel limit")) return false;
  replacement.channelPositions.clear();
  const auto empty = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  return check(!empty.error.empty() && empty.configuration == original,
               "a device without output channels must not erase the saved mapping");
}

static bool testDeviceOrderUsesChannelNumbers() {
  auto original = configuredOutputs();
  std::swap(original.channels[0], original.channels[2]);
  original.outputs[1].enabled = false;
  if (!check(pipetune_gtk::configuredOutputOrder(original) ==
                 std::vector<std::size_t>{1, 0},
             "disabled devices must sort by the first assigned channel"))
    return false;
  const auto unchanged = original;
  original.outputs.push_back({.id = "unassigned", .enabled = false,
                              .device = {.identity = {"node", "unused", "unused", "", "", ""},
                                         .name = "Unused", .profile = "Stereo",
                                         .channelPositions = {"FL"}}});
  return check(pipetune_gtk::configuredOutputOrder(original) ==
                   std::vector<std::size_t>{1, 0, 2} &&
                   original.outputs[0] == unchanged.outputs[0] &&
                   original.channels == unchanged.channels,
               "unassigned devices must follow assigned devices without changing the mapping");
}

static bool testDeviceRemovalKeepsChannelNumbers() {
  const auto original = configuredOutputs();
  auto expected = original;
  expected.outputs.erase(expected.outputs.begin());
  expected.channels[0] = {"", 0, "Main L"};
  expected.channels[1] = {"", 0, "Main R"};
  const auto removed = pipetune_gtk::removeOutputDevice(original, "a");
  if (!check(removed.error.empty() && removed.configuration == expected &&
                 original == configuredOutputs(),
             "removing an earlier device must reserve its numbers and labels"))
    return false;

  auto withoutReservedTail = original;
  withoutReservedTail.channels.pop_back();
  expected = withoutReservedTail;
  expected.outputs.pop_back();
  expected.channels.resize(2);
  const auto removedTail =
      pipetune_gtk::removeOutputDevice(withoutReservedTail, "b");
  if (!check(removedTail.error.empty() && removedTail.configuration == expected,
             "removing the final device channels must reduce the width"))
    return false;

  expected = original;
  expected.outputs.pop_back();
  expected.channels[2] = {"", 0, "Sub L"};
  expected.channels[3] = {"", 0, "Sub R"};
  const auto existingReservedTail =
      pipetune_gtk::removeOutputDevice(original, "b");
  if (!check(existingReservedTail.error.empty() &&
                 existingReservedTail.configuration == expected,
             "an existing trailing reservation must not be removed implicitly"))
    return false;

  auto onlyDevice = withoutReservedTail;
  onlyDevice.outputs.pop_back();
  onlyDevice.channels.resize(2);
  const auto removedLast = pipetune_gtk::removeOutputDevice(onlyDevice, "a");
  if (!check(removedLast.error.empty() && removedLast.configuration.outputs.empty() &&
                 removedLast.configuration.channels.empty() &&
                 removedLast.configuration.mode == pipetune::OutputMode::multiple,
             "the last device must be removable within an unfinished draft"))
    return false;
  const auto unknown = pipetune_gtk::removeOutputDevice(original, "unknown");
  return check(!unknown.error.empty() && unknown.configuration == original,
               "an unknown device must leave the configuration untouched");
}

static bool testExplicitChannelCompaction() {
  auto original = configuredOutputs();
  original.outputs[1].enabled = false;
  original.channels.insert(original.channels.begin() + 2, {"", 0, "Spare"});
  auto expected = original;
  expected.channels.erase(expected.channels.begin() + 2);
  expected.channels.pop_back();
  const auto compacted = pipetune_gtk::compactOutputChannels(original);
  if (!check(compacted.error.empty() && compacted.configuration == expected &&
                 original.channels.size() == 6,
             "compaction must preserve assignments to disabled devices and their relative order"))
    return false;

  auto full = original;
  full.channels.resize(16);
  const auto newDevice = pipetune::ConfiguredOutput{
      .id = "c", .enabled = true,
      .device = {.identity = {"node", "dac-c", "dac-c", "", "", ""},
                 .name = "DAC C", .profile = "Stereo",
                 .channelPositions = {"FL", "FR"}}};
  const auto before = pipetune::appendConfiguredOutput(full, newDevice);
  if (!check(!before.error.empty(),
             "sixteen occupied and reserved slots must block another device"))
    return false;
  const auto cleaned = pipetune_gtk::compactOutputChannels(full);
  const auto after = pipetune::appendConfiguredOutput(cleaned.configuration, newDevice);
  return check(cleaned.error.empty() && cleaned.configuration.channels.size() == 4 &&
                   after.error.empty() && after.configuration.channels.size() == 6,
               "explicit compaction must release reserved slots for a new device");
}

static bool testDisabledDraftRemainsEditable() {
  auto original = configuredOutputs();
  for (auto &output : original.outputs) output.enabled = false;
  auto replacement = original.outputs[0].device;
  replacement.identity.location = "replacement";
  replacement.name = "Replacement DAC";
  auto expected = original;
  expected.outputs[0].device = replacement;
  const auto replaced = pipetune_gtk::replaceOutputDevice(original, "a", replacement);
  if (!check(replaced.error.empty() && replaced.configuration == expected,
             "replacing a device in a disabled draft must preserve its disabled state"))
    return false;
  std::swap(expected.channels[0], expected.channels[1]);
  const auto moved = pipetune_gtk::moveOutputChannel(replaced.configuration, 0, 1);
  return check(moved.error.empty() && moved.configuration == expected &&
                   !pipetune::validateOutputConfiguration(moved.configuration).empty(),
               "a disabled draft must allow channel edits but remain invalid for confirmation");
}

int main() {
  auto success = testExplicitChannelMoves();
  success = testDeviceReplacement() && success;
  success = testInvalidReplacementsKeepOriginal() && success;
  success = testDeviceOrderUsesChannelNumbers() && success;
  success = testDeviceRemovalKeepsChannelNumbers() && success;
  success = testExplicitChannelCompaction() && success;
  success = testDisabledDraftRemainsEditable() && success;
  return success ? 0 : 1;
}
