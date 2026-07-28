#include "application-state.h"

#include "pipetune/control_protocol.h"

#include <array>
#include <iostream>
#include <span>
#include <string>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlResponseParseResult statusResponse(
    bool defaultSinkActive,
    std::span<const pipetune::ControlWarning> warnings,
    std::string_view configurationError) {
  return pipetune::parseControlResponse(
      pipetune::makeControlSuccessResponse(
          {.processingMode = pipetune::ProcessingMode::preset,
           .activePreset = "/tmp/active.effetune_preset",
           .configurationError = std::string(configurationError),
           .activePluginCount = 4,
           .selectedTarget = "alsa_output.speaker",
           .defaultSinkActive = defaultSinkActive,
           .overrunFrames = 0,
           .underrunFrames = 0,
           .processingErrors = 0,
           .inputSampleFormat = {},
           .inputSampleRate = 0,
           .inputChannelCount = 0,
           .inputFramesReceived = 0,
           .inputLastReceivedUnixMilliseconds = 0},
          warnings));
}

static pipetune::ControlResponseParseResult bypassStatusResponse() {
  return pipetune::parseControlResponse(
      pipetune::makeControlSuccessResponse(
          {.processingMode = pipetune::ProcessingMode::bypass,
           .activePreset = {},
           .configurationError = {},
           .activePluginCount = 0,
           .selectedTarget = "alsa_output.speaker",
           .defaultSinkActive = true,
           .overrunFrames = 0,
           .underrunFrames = 0,
           .processingErrors = 0,
           .inputSampleFormat = {},
           .inputSampleRate = 0,
           .inputChannelCount = 0,
           .inputFramesReceived = 0,
           .inputLastReceivedUnixMilliseconds = 0},
          {}));
}

static pipetune::ControlResponseParseResult inputStatusResponse(
    std::uint32_t sampleRate, std::uint64_t framesReceived) {
  return pipetune::parseControlResponse(
      pipetune::makeControlSuccessResponse(
          {.processingMode = pipetune::ProcessingMode::bypass,
           .activePreset = {},
           .configurationError = {},
           .activePluginCount = 0,
           .selectedTarget = "alsa_output.speaker",
           .defaultSinkActive = true,
           .overrunFrames = 0,
           .underrunFrames = 0,
           .processingErrors = 0,
           .inputSampleFormat = "F32P",
           .inputSampleRate = sampleRate,
           .inputChannelCount = 2,
           .inputFramesReceived = framesReceived,
           .inputLastReceivedUnixMilliseconds =
               framesReceived == 0 ? 0 : 1704164645000ULL},
          {}));
}

static bool testApplicationState() {
  auto state = pipetune_gtk::initialApplicationState();
  if (!check(state.connection ==
                 pipetune_gtk::ControlConnectionState::disconnected,
             "initial connection state differs") ||
      !check(!pipetune_gtk::isPresetApplied(state),
             "initial state must not confirm an applied preset") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::disconnected,
             "initial tray state differs")) {
    return false;
  }

  pipetune_gtk::markControlConnecting(state);
  if (!check(!pipetune_gtk::isPresetApplied(state),
             "connecting state must not confirm an applied preset")) {
    return false;
  }
  const auto healthy = statusResponse(true, {}, {});
  pipetune_gtk::applyControlResponse(state, healthy, 1000);
  if (!check(state.connection ==
                 pipetune_gtk::ControlConnectionState::connected,
             "valid status must connect the application") ||
      !check(state.hasRuntimeStatus &&
                 state.runtime.activePluginCount == 4,
             "runtime status was not retained") ||
      !check(pipetune_gtk::isPresetApplied(state),
             "preset runtime must confirm an applied preset") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::active,
             "healthy runtime must use the active tray state")) {
    return false;
  }

  auto reconnectingState = state;
  pipetune_gtk::markControlConnecting(reconnectingState);
  const auto connectionError = pipetune::parseControlResponse(
      pipetune::makeControlErrorResponse("subscription failed"));
  pipetune_gtk::applyControlResponse(reconnectingState, connectionError,
                                     2000);
  if (!check(!reconnectingState.hasRuntimeStatus &&
                 !pipetune_gtk::isPresetApplied(reconnectingState),
             "failed reconnection must invalidate the applied preset")) {
    return false;
  }

  auto bypassState = state;
  pipetune_gtk::applyControlResponse(bypassState, bypassStatusResponse(),
                                     2000);
  if (!check(!pipetune_gtk::isPresetApplied(bypassState),
             "bypass runtime must not confirm an applied preset")) {
    return false;
  }

  const auto warnings = std::array<pipetune::ControlWarning, 1>{
      pipetune::ControlWarning{.nodeIndex = 2,
                               .pluginName = "Unavailable DSP",
                               .reason = "not built"}};
  pipetune_gtk::applyControlResponse(
      state, statusResponse(true, warnings, {}), 3000);
  if (!check(state.warnings.size() == 1,
             "ordinary response warnings were not retained") ||
      !check(pipetune_gtk::isPresetApplied(state),
             "warnings must not hide an applied preset") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "warnings must request attention")) {
    return false;
  }

  const auto event = pipetune::parseControlResponse(
      pipetune::makeControlStatusEvent(healthy.status));
  pipetune_gtk::applyControlResponse(state, event, 4000);
  if (!check(state.warnings.size() == 1,
             "status events must not erase load warnings")) {
    return false;
  }
  pipetune_gtk::clearControlNotice(state);
  if (!check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::active,
             "clearing notices must restore a healthy tray state")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, statusResponse(false, {}, {}), 5000);
  if (!check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "an inactive default sink must request attention")) {
    return false;
  }
  pipetune_gtk::applyControlResponse(
      state, statusResponse(true, {}, "configured preset is unavailable"),
      6000);
  if (!check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "a startup configuration error must request attention")) {
    return false;
  }

  pipetune_gtk::markControlDisconnected(state, "daemon stopped");
  return check(state.connection ==
                   pipetune_gtk::ControlConnectionState::disconnected,
               "disconnect state differs") &&
         check(state.diagnostic == "daemon stopped",
               "disconnect diagnostic differs") &&
         check(!pipetune_gtk::isPresetApplied(state),
               "disconnection must invalidate the applied preset") &&
         check(pipetune_gtk::trayVisualState(state) ==
                   pipetune_gtk::TrayVisualState::disconnected,
               "disconnected tray state differs");
}

static bool testInputRateState() {
  auto state = pipetune_gtk::initialApplicationState();
  pipetune_gtk::markControlConnecting(state);
  pipetune_gtk::applyControlResponse(
      state, inputStatusResponse(48000, 0), 1000);
  if (!check(state.inputRate.hasBaseline,
             "first input status must establish a baseline") ||
      !check(!state.inputRate.hasRate,
             "first input status must not invent a frame rate")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, inputStatusResponse(48000, 9600), 1200);
  if (!check(!state.inputRate.hasRate,
             "short status interval must not produce a frame rate") ||
      !check(state.inputRate.baselineFrames == 0,
             "short status interval must preserve the baseline")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, inputStatusResponse(48000, 48000), 2000);
  if (!check(state.inputRate.hasRate,
             "one-second status interval must produce a frame rate") ||
      !check(state.inputRate.framesPerSecond == 48000.0,
             "calculated input frame rate differs")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, inputStatusResponse(48000, 48000), 3000);
  if (!check(state.inputRate.framesPerSecond == 0.0,
             "unchanged input counter must report zero flow")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, inputStatusResponse(48000, 100), 4000);
  if (!check(!state.inputRate.hasRate,
             "decreasing input counter must reset the rate") ||
      !check(state.inputRate.baselineFrames == 100,
             "decreasing input counter must establish a new baseline")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, inputStatusResponse(44100, 44200), 5000);
  if (!check(!state.inputRate.hasRate,
             "format change must reset the input rate")) {
    return false;
  }

  pipetune_gtk::markControlConnecting(state);
  return check(!state.inputRate.hasBaseline && !state.inputRate.hasRate,
               "reconnection must reset input rate measurements");
}

int main() {
  return testApplicationState() && testInputRateState() ? 0 : 1;
}
