/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_paths.h"

#include <iostream>
#include <limits>

static bool check(bool condition, const char *message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

int main() {
  auto configuration = pipetune::OutputConfiguration{};
  for (const auto *id : {"a", "b"}) {
    auto result = pipetune::appendConfiguredOutput(configuration, {
        .id = id, .enabled = true, .device = {.identity = {.api = "node", .location = id, .port = id,
            .vendor = {}, .product = {}, .serial = {}}, .name = id, .profile = {}, .channelPositions = {"FL", "FR"}}});
    if (!check(result.error.empty(), "fixture must describe two complete stereo outputs")) return 1;
    configuration = std::move(result.configuration);
  }
  configuration.mode = pipetune::OutputMode::multiple;
  const auto inventory = std::vector<pipetune::AvailableOutput>{
      {configuration.outputs[1].device, "b", 20, 200}, {configuration.outputs[0].device, "a", 10, 100}};
  const auto paths = std::vector<pipetune::PipeWireOutputPath>{
      {.outputId = "b", .channelSlots = {2, 3}, .streamSerial = 201, .targetSerial = 200, .targetNodeId = 20,
       .activity = pipetune::OutputPathActivity::active, .reportedLatencyNanoseconds = 3000000},
      {.outputId = "a", .channelSlots = {0, 1}, .streamSerial = 101, .targetSerial = 100, .targetNodeId = 10,
       .activity = pipetune::OutputPathActivity::active, .reportedLatencyNanoseconds = 1000000}};
  auto timings = pipetune::summarizeOutputTiming(configuration, inventory, paths);
  if (!check(timings.size() == 2, "every resolved output needs a timing entry")) return 1;
  if (!check(timings[0].outputId == "a" && timings[0].nodeSerial == 100 &&
      timings[0].estimatedCompensationNanoseconds == 2000000 && timings[1].estimatedCompensationNanoseconds == 0,
      "the faster output needs two milliseconds, independent of enumeration order")) return 1;
  for (auto kind = 0; kind < 7; ++kind) {
    auto changed = paths;
    if (kind == 0) changed[0].reportedLatencyNanoseconds.reset();
    if (kind == 1) changed[0].activity = pipetune::OutputPathActivity::idle;
    if (kind == 2) changed[0].targetSerial = 199;
    if (kind == 3) changed[0].channelSlots = {3, 2};
    if (kind == 4) { auto duplicate = changed[0]; ++duplicate.streamSerial; changed.push_back(duplicate); }
    if (kind == 5) changed[0].reportedLatencyNanoseconds = -1;
    if (kind == 6) changed[0].reportedLatencyNanoseconds = std::numeric_limits<double>::quiet_NaN();
    timings = pipetune::summarizeOutputTiming(configuration, inventory, changed);
    if (!check(timings.size() == 2 && !timings[0].estimatedCompensationNanoseconds && !timings[1].estimatedCompensationNanoseconds,
        "unknown, inactive, stale, remapped, duplicated or invalid paths must suppress estimates")) return 1;
    if (kind >= 2 && kind <= 4 && !check(timings[1].activity == pipetune::OutputPathActivity::pending,
        "another generation or channel mapping must not confirm the current route")) return 1;
  }
  auto changed = configuration;
  changed.outputs[1].enabled = false;
  timings = pipetune::summarizeOutputTiming(changed, inventory, paths);
  if (!check(timings.size() == 1 && timings[0].outputId == "a" && timings[0].estimatedCompensationNanoseconds == 0,
      "a disabled slower output must not remain the delay reference")) return 1;
  timings = pipetune::summarizeOutputTiming(configuration, std::span(inventory).subspan(1), paths);
  if (!check(timings.size() == 1 && timings[0].estimatedCompensationNanoseconds == 0,
      "a disconnected slower output must not remain the delay reference")) return 1;
  changed = configuration;
  changed.mode = pipetune::OutputMode::single;
  return check(pipetune::summarizeOutputTiming(changed, inventory, paths).empty(),
      "single mode must not publish inactive multiple-output timing") ? 0 : 1;
}
