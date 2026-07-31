#include "status-text.h"

#include "localization.h"
#include "ui-message.h"

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
  if (state.runtime.pipeWireIdle ||
      state.runtime.dspIdleState == pipetune::DspIdleState::sleeping ||
      !state.dspTiming.hasAverage ||
      !std::isfinite(state.dspTiming.nanosecondsPerFrame) ||
      state.dspTiming.nanosecondsPerFrame < 0.0) {
    return "—";
  }
  const auto processingTime =
      fixedDecimal(state.dspTiming.nanosecondsPerFrame / 1000.0, 2) +
      " µs/frame";
  if (state.runtime.inputSampleRate == 0) {
    return formatUiMessage(localizedMessage(
        "{0}  •  Load —", {processingTime}));
  }
  const auto frameBudgetNanoseconds =
      kNanosecondsPerSecond /
      static_cast<double>(state.runtime.inputSampleRate);
  const auto loadPercentage =
      state.dspTiming.nanosecondsPerFrame / frameBudgetNanoseconds * 100.0;
  return formatUiMessage(localizedMessage(
      "{0}  •  Load {1}%",
      {processingTime, fixedDecimal(loadPercentage, 1)}));
}

static std::string frameRateText(const InputRateState &inputRate) {
  if (!inputRate.hasRate || !std::isfinite(inputRate.framesPerSecond) ||
      inputRate.framesPerSecond < 0.0) {
    return "—";
  }
  const auto rounded =
      static_cast<std::uint64_t>(std::llround(inputRate.framesPerSecond));
  return formatUiMessage(localizedMessage(
      "{0} frames/s", {groupedInteger(rounded)}));
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
          ? translate("32-bit floating-point PCM (planar)")
          : translate("Unknown sample format");
  const auto *channelTemplate = translatePlural(
      "{0} channel", "{0} channels", status.inputChannelCount);
  const auto channelCount = formatUiMessage(
      {.translatable = false,
       .messageId = channelTemplate,
       .arguments = {std::to_string(status.inputChannelCount)}});
  return formatUiMessage(localizedMessage(
      "{0} · {1} kHz · {2}",
      {sampleFormat, sampleRate, channelCount}));
}

static std::string relativeAgeText(std::uint64_t ageMilliseconds) {
  if (ageMilliseconds < kMillisecondsPerSecond) {
    return formatUiMessage(localizedMessage(
        "{0} ms ago", {std::to_string(ageMilliseconds)}));
  }
  if (ageMilliseconds < kMillisecondsPerMinute) {
    return formatUiMessage(localizedMessage(
        "{0} s ago",
        {std::to_string(ageMilliseconds / kMillisecondsPerSecond)}));
  }
  if (ageMilliseconds < kMillisecondsPerHour) {
    return formatUiMessage(localizedMessage(
        "{0} min ago",
        {std::to_string(ageMilliseconds / kMillisecondsPerMinute)}));
  }
  return formatUiMessage(localizedMessage(
      "{0} h ago",
      {std::to_string(ageMilliseconds / kMillisecondsPerHour)}));
}

static std::string lastReceivedText(std::uint64_t receivedUnixMilliseconds,
                                    std::uint64_t currentUnixMilliseconds) {
  if (receivedUnixMilliseconds == 0) {
    return translate("Never");
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
  return formatUiMessage(localizedMessage(
      "{0} ({1})", {stream.str(), relativeAgeText(age)}));
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
      .counters = formatUiMessage(localizedMessage(
          "Overrun {0}  •  Underrun {1}  •  Processing {2}",
          {std::to_string(state.runtime.overrunFrames),
           std::to_string(state.runtime.underrunFrames),
           std::to_string(state.runtime.processingErrors)})),
  };
}

} // namespace pipetune_gtk
