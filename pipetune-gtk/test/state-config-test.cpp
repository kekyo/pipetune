#include "application-state.h"
#include "startup-config.h"

#include "pipetune/control_protocol.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlResponseParseResult statusResponse(
    bool defaultSinkActive,
    std::span<const pipetune::ControlWarning> warnings) {
  return pipetune::parseControlResponse(
      pipetune::makeControlSuccessResponse(
          {.processingMode = pipetune::ProcessingMode::preset,
           .activePreset = "/tmp/active.effetune_preset",
           .configurationError = {},
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
  const auto healthy = statusResponse(true, {});
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
  pipetune_gtk::applyControlResponse(state, statusResponse(true, warnings));
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

  pipetune_gtk::applyControlResponse(state, statusResponse(false, {}));
  if (!check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "an inactive default sink must request attention")) {
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

static bool testStartupConfig() {
  const auto explicitPath = pipetune_gtk::resolveStartupConfigPath(
      "/tmp/xdg-config", "/tmp/home");
  const auto fallbackPath =
      pipetune_gtk::resolveStartupConfigPath({}, "/tmp/home");
  if (!check(explicitPath.error.empty() &&
                 explicitPath.path ==
                     "/tmp/xdg-config/pipetune/environment.gtk",
             "XDG startup config path differs") ||
      !check(fallbackPath.error.empty() &&
                 fallbackPath.path ==
                     "/tmp/home/.config/pipetune/environment.gtk",
             "fallback startup config path differs")) {
    return false;
  }

  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-gtk-config-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto configPath = directory / "pipetune" / "environment.gtk";
  const auto presetPath =
      std::filesystem::path("/tmp/Music \"wide\" \\\\ room.effetune_preset");
  const auto saved =
      pipetune_gtk::saveStartupPreset(configPath, presetPath);
  if (!check(saved.empty(), saved)) {
    std::filesystem::remove_all(directory);
    return false;
  }

  struct stat status {};
  const auto loaded = pipetune_gtk::loadStartupPreset(configPath);
  const auto valid =
      check(loaded.error.empty(), loaded.error) &&
      check(loaded.found, "saved startup preset was not found") &&
      check(loaded.presetPath == presetPath,
            "startup preset did not round-trip") &&
      check(stat(configPath.c_str(), &status) == 0 &&
                (status.st_mode & 0777) == 0600,
            "startup override must be private");
  const auto invalid =
      pipetune_gtk::saveStartupPreset(configPath, "relative.effetune_preset");
  const auto stillLoaded = pipetune_gtk::loadStartupPreset(configPath);
  const auto preserved =
      check(!invalid.empty(), "relative presets must be rejected") &&
      check(stillLoaded.error.empty() && stillLoaded.found &&
                stillLoaded.presetPath == presetPath,
            "a rejected save must preserve the previous startup preset");
  std::filesystem::remove_all(directory);
  return valid && preserved;
}

int main() {
  return testApplicationState() && testStartupConfig() ? 0 : 1;
}
