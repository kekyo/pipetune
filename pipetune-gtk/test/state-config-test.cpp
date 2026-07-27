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
           .processingErrors = 0},
          warnings));
}

static bool testApplicationState() {
  auto state = pipetune_gtk::initialApplicationState();
  if (!check(state.connection ==
                 pipetune_gtk::ControlConnectionState::disconnected,
             "initial connection state differs") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::disconnected,
             "initial tray state differs")) {
    return false;
  }

  pipetune_gtk::markControlConnecting(state);
  const auto healthy = statusResponse(true, {}, {});
  pipetune_gtk::applyControlResponse(state, healthy);
  if (!check(state.connection ==
                 pipetune_gtk::ControlConnectionState::connected,
             "valid status must connect the application") ||
      !check(state.hasRuntimeStatus &&
                 state.runtime.activePluginCount == 4,
             "runtime status was not retained") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::active,
             "healthy runtime must use the active tray state")) {
    return false;
  }

  const auto warnings = std::array<pipetune::ControlWarning, 1>{
      pipetune::ControlWarning{.nodeIndex = 2,
                               .pluginName = "Unavailable DSP",
                               .reason = "not built"}};
  pipetune_gtk::applyControlResponse(state,
                                     statusResponse(true, warnings, {}));
  if (!check(state.warnings.size() == 1,
             "ordinary response warnings were not retained") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "warnings must request attention")) {
    return false;
  }

  const auto event = pipetune::parseControlResponse(
      pipetune::makeControlStatusEvent(healthy.status));
  pipetune_gtk::applyControlResponse(state, event);
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

  pipetune_gtk::applyControlResponse(state, statusResponse(false, {}, {}));
  if (!check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "an inactive default sink must request attention")) {
    return false;
  }
  pipetune_gtk::applyControlResponse(
      state, statusResponse(true, {}, "configured preset is unavailable"));
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
         check(pipetune_gtk::trayVisualState(state) ==
                   pipetune_gtk::TrayVisualState::disconnected,
               "disconnected tray state differs");
}

int main() {
  return testApplicationState() ? 0 : 1;
}
