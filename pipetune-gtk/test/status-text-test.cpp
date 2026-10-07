/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "application-state.h"
#include "status-text.h"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune_gtk::ApplicationState activeState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime.inputSampleFormat = "F32P";
  state.runtime.inputSampleRate = 48000;
  state.runtime.inputChannelCount = 2;
  state.runtime.inputFramesReceived = 48000;
  state.runtime.inputLastReceivedUnixMilliseconds = 1704164645000ULL;
  state.inputRate.hasRate = true;
  state.inputRate.framesPerSecond = 48003.4;
  return state;
}

static bool testActiveText() {
  const auto previousTimezone = std::getenv("TZ");
  const auto savedTimezone =
      previousTimezone == nullptr
          ? std::optional<std::string>{}
          : std::optional<std::string>{previousTimezone};
  setenv("TZ", "UTC", 1);
  tzset();

  const auto text = pipetune_gtk::inputStatusText(
      activeState(), 1704164645012ULL);

  if (savedTimezone.has_value()) {
    setenv("TZ", savedTimezone->c_str(), 1);
  } else {
    unsetenv("TZ");
  }
  tzset();

  return check(text.frameRate == "48,003 frames/s",
               "input frame-rate text differs") &&
         check(text.lastReceived == "03:04:05 (12 ms ago)",
               "last-input text differs") &&
         check(text.pcmDataRate == "3.07 Mbit/s",
               "PCM data-rate text differs") &&
         check(text.streamFormat ==
                   "32-bit floating-point PCM (planar) · "
                   "48 kHz · 2 channels",
               "stream-format text differs");
}

static bool testUnavailableAndIdleText() {
  const auto disconnected = pipetune_gtk::inputStatusText(
      pipetune_gtk::initialApplicationState(), 1704164645000ULL);
  if (!check(disconnected.frameRate == "—" &&
                 disconnected.lastReceived == "—" &&
                 disconnected.pcmDataRate == "—" &&
                 disconnected.streamFormat == "—",
             "disconnected input text must be unavailable")) {
    return false;
  }

  auto idle = activeState();
  idle.inputRate.framesPerSecond = 0.0;
  idle.runtime.inputLastReceivedUnixMilliseconds = 0;
  idle.runtime.inputSampleRate = 44100;
  const auto idleText =
      pipetune_gtk::inputStatusText(idle, 1704164645000ULL);
  auto mono = idle;
  mono.runtime.inputChannelCount = 1;
  const auto monoText =
      pipetune_gtk::inputStatusText(mono, 1704164645000ULL);
  return check(idleText.frameRate == "0 frames/s",
               "idle frame-rate text differs") &&
         check(idleText.lastReceived == "Never",
               "never-received text differs") &&
         check(idleText.pcmDataRate == "0 bit/s",
               "idle PCM data-rate text differs") &&
         check(idleText.streamFormat ==
                   "32-bit floating-point PCM (planar) · "
                   "44.1 kHz · 2 channels",
               "44.1 kHz stream-format text differs") &&
         check(monoText.streamFormat ==
                   "32-bit floating-point PCM (planar) · "
                   "44.1 kHz · 1 channel",
               "mono stream-format text differs");
}

static bool testRuntimeText() {
  const auto unavailable =
      pipetune_gtk::runtimeStatusText(
          pipetune_gtk::initialApplicationState());
  if (!check(unavailable.dspProcessingTime == "—" &&
                 unavailable.counters == "—",
             "disconnected runtime text must be unavailable")) {
    return false;
  }

  auto state = activeState();
  state.dspTiming.hasAverage = true;
  state.dspTiming.nanosecondsPerFrame = 1000.0;
  state.runtime.inputSampleRate = 48000;
  state.runtime.dspSampleRate = 200000;
  state.runtime.overrunFrames = 4;
  state.runtime.underrunFrames = 5;
  state.runtime.processingErrors = 6;
  const auto runtime = pipetune_gtk::runtimeStatusText(state);
  auto overloaded = state;
  overloaded.dspTiming.nanosecondsPerFrame = 6000.0;
  const auto overloadedRuntime =
      pipetune_gtk::runtimeStatusText(overloaded);
  return check(runtime.dspProcessingTime ==
                   "1.00 µs/frame  •  Load 20.0%",
               "DSP processing-time text differs") &&
         check(overloadedRuntime.dspProcessingTime ==
                   "6.00 µs/frame  •  Load 120.0%",
               "overloaded DSP processing-time text differs") &&
         check(runtime.counters ==
                   "Overrun 4  •  Underrun 5  •  Processing 6",
               "runtime counter text differs");
}

static bool testOutputVolumeText() {
  const auto unknown = std::string("Device mute unknown · Scalar gain: Unknown · Channel gains: Unknown");
  auto volume = pipetune::OutputVolumeState{};
  if (!check(pipetune_gtk::outputVolumeText(nullptr) == unknown &&
             pipetune_gtk::outputVolumeText(&volume) == unknown,
             "unavailable device controls must remain unknown")) return false;
  volume = {73, true, 1, {0.25F, 0.5F}};
  if (!check(pipetune_gtk::outputVolumeText(&volume) ==
             "Device muted · Scalar gain: 0.0 dB · Channel gains: -12.0 dB … -6.0 dB",
             "mute and unequal channel gains must remain distinct from the master volume")) return false;
  volume = {73, false, {}, {0, 2}};
  if (!check(pipetune_gtk::outputVolumeText(&volume) ==
             "Device unmuted · Scalar gain: Unknown · Channel gains: -∞ dB … 6.0 dB",
             "zero and amplified channel gains must be reported without clamping")) return false;
  volume = {73, false, 0.5F, {1, 1, 1}};
  return check(pipetune_gtk::outputVolumeText(&volume) ==
             "Device unmuted · Scalar gain: -6.0 dB · Channel gains: 0.0 dB",
             "uniform channel gains must not be multiplied by a separate scalar report");
}

static bool testOutputTimingText() {
  using pipetune::OutputPathActivity;
  if (!check(pipetune_gtk::outputTimingText(nullptr) ==
             "Audio path unavailable · Estimated compensation: Unknown",
             "missing timing must not imply a running path or zero compensation")) return false;
  auto timing = pipetune::OutputTimingState{"dac-a", 73, OutputPathActivity::active, 1'000'000, 2'125'000};
  if (!check(pipetune_gtk::outputTimingText(&timing) ==
             "Audio path active · Estimated compensation: 2.125 ms",
             "compensation must be identified as an estimate in milliseconds")) return false;
  timing.estimatedCompensationNanoseconds = 0;
  if (!check(pipetune_gtk::outputTimingText(&timing) ==
             "Audio path active · Estimated compensation: 0.000 ms",
             "a known zero estimate must remain distinct from unknown")) return false;
  for (const auto value : {std::optional<double>{}, std::optional<double>{-1},
                           std::optional<double>{std::numeric_limits<double>::infinity()},
                           std::optional<double>{std::numeric_limits<double>::quiet_NaN()}}) {
    timing.estimatedCompensationNanoseconds = value;
    if (!check(pipetune_gtk::outputTimingText(&timing) ==
               "Audio path active · Estimated compensation: Unknown",
               "missing or invalid estimates must not alter observed activity")) return false;
  }
  timing.estimatedCompensationNanoseconds = 2'125'000;
  for (const auto &[activity, text] : {
      std::pair{OutputPathActivity::pending, "Audio path pending · Estimated compensation: Unknown"},
      std::pair{OutputPathActivity::idle, "Audio path idle · Estimated compensation: Unknown"},
      std::pair{OutputPathActivity::error, "Audio path error · Estimated compensation: Unknown"}}) {
    timing.activity = activity;
    if (!check(pipetune_gtk::outputTimingText(&timing) == text,
               "inactive paths must not retain an estimate")) return false;
  }
  return true;
}

int main() {
  if (!testOutputTimingText()) return 1;
  if (!testOutputVolumeText()) return 1;
  return testActiveText() && testUnavailableAndIdleText() &&
                 testRuntimeText()
             ? 0
             : 1;
}
