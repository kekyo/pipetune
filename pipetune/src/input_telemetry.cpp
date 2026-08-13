/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "input_telemetry.h"

#include <algorithm>
#include <cstdint>

namespace pipetune {

constexpr auto kNanosecondsPerMillisecond = std::int64_t{1'000'000};

InputTelemetry::InputTelemetry() noexcept
    : framesReceived(0), lastReceivedMonotonicNanoseconds(0) {}

void recordInputFrames(InputTelemetry &telemetry, std::uint32_t frameCount,
                       std::int64_t monotonicNanoseconds) noexcept {
  if (frameCount == 0) {
    return;
  }
  telemetry.lastReceivedMonotonicNanoseconds.store(
      monotonicNanoseconds, std::memory_order_relaxed);
  telemetry.framesReceived.fetch_add(frameCount, std::memory_order_release);
}

InputTelemetrySnapshot snapshotInputTelemetry(
    const InputTelemetry &telemetry, std::int64_t currentMonotonicNanoseconds,
    std::uint64_t currentUnixMilliseconds) noexcept {
  const auto frames =
      telemetry.framesReceived.load(std::memory_order_acquire);
  const auto lastMonotonic =
      telemetry.lastReceivedMonotonicNanoseconds.load(
          std::memory_order_relaxed);
  if (frames == 0 || lastMonotonic <= 0) {
    return {.framesReceived = frames,
            .lastReceivedUnixMilliseconds = 0};
  }

  const auto ageNanoseconds =
      std::max(std::int64_t{0},
               currentMonotonicNanoseconds - lastMonotonic);
  const auto ageMilliseconds =
      static_cast<std::uint64_t>(ageNanoseconds /
                                 kNanosecondsPerMillisecond);
  return {
      .framesReceived = frames,
      .lastReceivedUnixMilliseconds =
          ageMilliseconds > currentUnixMilliseconds
              ? std::uint64_t{0}
              : currentUnixMilliseconds - ageMilliseconds,
  };
}

} // namespace pipetune
