/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "input_telemetry.h"

#include <cstdint>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testInputTelemetry() {
  auto telemetry = pipetune::InputTelemetry{};
  const auto empty =
      pipetune::snapshotInputTelemetry(telemetry, 2'000'000'000, 10'000);
  if (!check(empty.framesReceived == 0,
             "empty telemetry must report zero frames") ||
      !check(empty.lastReceivedUnixMilliseconds == 0,
             "empty telemetry must not report a receive time")) {
    return false;
  }

  pipetune::recordInputFrames(telemetry, 0, 1'000'000'000);
  pipetune::recordInputFrames(telemetry, 480, 1'000'000'000);
  pipetune::recordInputFrames(telemetry, 520, 1'500'000'000);
  const auto populated =
      pipetune::snapshotInputTelemetry(telemetry, 2'000'000'000, 10'000);
  if (!check(populated.framesReceived == 1000,
             "telemetry frame total differs") ||
      !check(populated.lastReceivedUnixMilliseconds == 9'500,
             "telemetry receive time differs")) {
    return false;
  }

  const auto clockAdjusted =
      pipetune::snapshotInputTelemetry(telemetry, 1'000'000'000, 10'000);
  return check(clockAdjusted.lastReceivedUnixMilliseconds == 10'000,
               "negative monotonic age must clamp to zero");
}

int main() {
  return testInputTelemetry() ? 0 : 1;
}
