/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "input_telemetry.h"
#include "output_peak_telemetry.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>
#include <vector>

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

static bool testOutputPeaks() {
  auto telemetry = pipetune::OutputPeakTelemetry{};
  const auto first = std::array<float, 4>{0.25F, -1.25F,
      std::numeric_limits<float>::quiet_NaN(), 0.5F};
  const auto second = std::array<float, 2>{0.4F, -0.8F};
  pipetune::recordOutputPeak(telemetry, 0, first);
  pipetune::recordOutputPeak(telemetry, 1, second);
  pipetune::publishOutputPeaks(telemetry);
  if (!check(pipetune::snapshotOutputPeaks(telemetry, 2) ==
                 std::vector<float>{1.25F, 0.8F},
             "output peaks must retain independent channel maxima above full scale")) return false;

  pipetune::recordOutputPeak(telemetry, 0, std::array<float, 1>{0.2F});
  pipetune::publishOutputPeaks(telemetry);
  if (!check(pipetune::snapshotOutputPeaks(telemetry, 2) ==
                 std::vector<float>{0.2F, 0.0F},
             "a silent channel must clear on the next publication")) return false;
  pipetune::publishOutputPeaks(telemetry);
  return check(pipetune::snapshotOutputPeaks(telemetry, 2) ==
                   std::vector<float>{0.0F, 0.0F},
               "stopped audio must clear stale peaks");
}

int main() {
  return testInputTelemetry() && testOutputPeaks() ? 0 : 1;
}
