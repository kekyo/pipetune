#include "status-text.h"

#include <cmath>
#include <cstdint>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace pipetune_gtk {

constexpr auto kBitsPerFloatSample = std::uint32_t{32};
constexpr auto kNanosecondsPerSecond = 1'000'000'000.0;
constexpr auto kMillisecondsPerSecond = std::uint64_t{1000};
constexpr auto kMillisecondsPerMinute =
    std::uint64_t{60} * kMillisecondsPerSecond;
constexpr auto kMillisecondsPerHour =
    std::uint64_t{60} * kMillisecondsPerMinute;

static std::string groupedInteger(std::uint64_t value) {
  const auto digits = std::to_string(value);
  auto result = std::string{};
  result.reserve(digits.size() + digits.size() / 3);
  for (auto index = std::size_t{0}; index < digits.size(); ++index) {
    if (index != 0 && (digits.size() - index) % 3 == 0) {
      result.push_back(',');
    }
    result.push_back(digits[index]);
  }
  return result;
}

static std::string trimmedDecimal(double value, int precision) {
  auto stream = std::ostringstream{};
  stream << std::fixed << std::setprecision(precision) << value;
  auto result = stream.str();
  while (!result.empty() && result.back() == '0') {
    result.pop_back();
  }
  if (!result.empty() && result.back() == '.') {
    result.pop_back();
  }
  return result;
}

static std::string fixedDecimal(double value, int precision) {
  auto stream = std::ostringstream{};
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

static std::string dspProcessingTimeText(
    const ApplicationState &state) {
  if (!state.dspTiming.hasAverage ||
      !std::isfinite(state.dspTiming.nanosecondsPerFrame) ||
      state.dspTiming.nanosecondsPerFrame < 0.0) {
    return "—";
  }
  const auto processingTime =
      fixedDecimal(state.dspTiming.nanosecondsPerFrame / 1000.0, 2) +
      " µs/frame";
  if (state.runtime.inputSampleRate == 0) {
    return processingTime + "  •  Load —";
  }
  const auto frameBudgetNanoseconds =
      kNanosecondsPerSecond /
      static_cast<double>(state.runtime.inputSampleRate);
  const auto loadPercentage =
      state.dspTiming.nanosecondsPerFrame / frameBudgetNanoseconds * 100.0;
  return processingTime + "  •  Load " +
         fixedDecimal(loadPercentage, 1) + "%";
}

static std::string frameRateText(const InputRateState &inputRate) {
  if (!inputRate.hasRate || !std::isfinite(inputRate.framesPerSecond) ||
      inputRate.framesPerSecond < 0.0) {
    return "—";
  }
  const auto rounded =
      static_cast<std::uint64_t>(std::llround(inputRate.framesPerSecond));
  return groupedInteger(rounded) + " frames/s";
}

static std::string pcmDataRateText(const ApplicationState &state) {
  if (!state.inputRate.hasRate ||
      state.runtime.inputChannelCount == 0 ||
      !std::isfinite(state.inputRate.framesPerSecond) ||
      state.inputRate.framesPerSecond < 0.0) {
    return "—";
  }
  const auto bitsPerSecond =
      state.inputRate.framesPerSecond *
      static_cast<double>(state.runtime.inputChannelCount) *
      static_cast<double>(kBitsPerFloatSample);
  if (bitsPerSecond >= 1'000'000.0) {
    return trimmedDecimal(bitsPerSecond / 1'000'000.0, 2) +
           " Mbit/s";
  }
  if (bitsPerSecond >= 1'000.0) {
    return trimmedDecimal(bitsPerSecond / 1'000.0, 2) +
           " kbit/s";
  }
  return groupedInteger(
             static_cast<std::uint64_t>(std::llround(bitsPerSecond))) +
         " bit/s";
}

static std::string streamFormatText(
    const pipetune::ControlRuntimeStatus &status) {
  if (status.inputSampleFormat.empty() || status.inputSampleRate == 0 ||
      status.inputChannelCount == 0) {
    return "—";
  }
  const auto sampleRate =
      trimmedDecimal(static_cast<double>(status.inputSampleRate) / 1000.0,
                     3);
  const auto sampleFormat =
      status.inputSampleFormat == "F32P"
          ? std::string_view("32-bit floating-point PCM (planar)")
          : std::string_view("Unknown sample format");
  const auto channelCount =
      std::to_string(status.inputChannelCount) +
      (status.inputChannelCount == 1 ? " channel" : " channels");
  return std::string(sampleFormat) + " · " + sampleRate + " kHz · " +
         channelCount;
}

static std::string relativeAgeText(std::uint64_t ageMilliseconds) {
  if (ageMilliseconds < kMillisecondsPerSecond) {
    return std::to_string(ageMilliseconds) + " ms ago";
  }
  if (ageMilliseconds < kMillisecondsPerMinute) {
    return std::to_string(ageMilliseconds / kMillisecondsPerSecond) +
           " s ago";
  }
  if (ageMilliseconds < kMillisecondsPerHour) {
    return std::to_string(ageMilliseconds / kMillisecondsPerMinute) +
           " min ago";
  }
  return std::to_string(ageMilliseconds / kMillisecondsPerHour) +
         " h ago";
}

static std::string lastReceivedText(std::uint64_t receivedUnixMilliseconds,
                                    std::uint64_t currentUnixMilliseconds) {
  if (receivedUnixMilliseconds == 0) {
    return "Never";
  }
  const auto receivedSeconds =
      static_cast<std::time_t>(receivedUnixMilliseconds /
                               kMillisecondsPerSecond);
  auto local = std::tm{};
  if (localtime_r(&receivedSeconds, &local) == nullptr) {
    return "—";
  }
  auto stream = std::ostringstream{};
  stream << std::put_time(&local, "%H:%M:%S");
  const auto age =
      currentUnixMilliseconds >= receivedUnixMilliseconds
          ? currentUnixMilliseconds - receivedUnixMilliseconds
          : std::uint64_t{0};
  return stream.str() + " (" + relativeAgeText(age) + ")";
}

InputStatusText inputStatusText(const ApplicationState &state,
                                std::uint64_t currentUnixMilliseconds) {
  if (state.connection != ControlConnectionState::connected ||
      !state.hasRuntimeStatus) {
    return {.frameRate = "—",
            .lastReceived = "—",
            .pcmDataRate = "—",
            .streamFormat = "—"};
  }
  return {
      .frameRate = frameRateText(state.inputRate),
      .lastReceived = lastReceivedText(
          state.runtime.inputLastReceivedUnixMilliseconds,
          currentUnixMilliseconds),
      .pcmDataRate = pcmDataRateText(state),
      .streamFormat = streamFormatText(state.runtime),
  };
}

RuntimeStatusText runtimeStatusText(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected ||
      !state.hasRuntimeStatus) {
    return {.dspProcessingTime = "—", .counters = "—"};
  }
  return {
      .dspProcessingTime = dspProcessingTimeText(state),
      .counters =
          "Overrun " + std::to_string(state.runtime.overrunFrames) +
          "  •  Underrun " +
          std::to_string(state.runtime.underrunFrames) +
          "  •  Processing " +
          std::to_string(state.runtime.processingErrors),
  };
}

} // namespace pipetune_gtk
