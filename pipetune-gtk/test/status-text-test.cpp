#include "application-state.h"
#include "status-text.h"

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

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
         check(text.streamFormat == "F32P · 48 kHz · 2 ch",
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
  return check(idleText.frameRate == "0 frames/s",
               "idle frame-rate text differs") &&
         check(idleText.lastReceived == "Never",
               "never-received text differs") &&
         check(idleText.pcmDataRate == "0 bit/s",
               "idle PCM data-rate text differs") &&
         check(idleText.streamFormat == "F32P · 44.1 kHz · 2 ch",
               "44.1 kHz stream-format text differs");
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
  state.dspTiming.nanosecondsPerFrame = 2500.0;
  state.runtime.overrunFrames = 4;
  state.runtime.underrunFrames = 5;
  state.runtime.processingErrors = 6;
  const auto runtime = pipetune_gtk::runtimeStatusText(state);
  return check(runtime.dspProcessingTime == "2.50 µs/frame",
               "DSP processing-time text differs") &&
         check(runtime.counters ==
                   "Overrun 4  •  Underrun 5  •  Processing 6",
               "runtime counter text differs");
}

int main() {
  return testActiveText() && testUnavailableAndIdleText() &&
                 testRuntimeText()
             ? 0
             : 1;
}
