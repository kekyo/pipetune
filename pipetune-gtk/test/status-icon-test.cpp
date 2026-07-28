#include "status-icon.h"

#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune_gtk::ApplicationState connectedState(
    pipetune::ProcessingMode mode) {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime.processingMode = mode;
  state.runtime.activePreset =
      mode == pipetune::ProcessingMode::preset
          ? "/tmp/active.effetune_preset"
          : "";
  state.runtime.selectedTarget = "alsa_output.speaker";
  state.runtime.defaultSinkActive = true;
  return state;
}

int main() {
  const auto disconnected = pipetune_gtk::statusIconPresentation(
      pipetune_gtk::initialApplicationState());
  if (!check(disconnected.colorMode ==
                 pipetune_gtk::TrayIconColorMode::grayscale,
             "disconnected icon must be grayscale") ||
      !check(disconnected.badge ==
                 pipetune_gtk::StatusBadge::disconnected,
             "disconnected icon badge differs")) {
    return 1;
  }

  auto connectingState = pipetune_gtk::initialApplicationState();
  pipetune_gtk::markControlConnecting(connectingState);
  const auto connecting =
      pipetune_gtk::statusIconPresentation(connectingState);
  if (!check(connecting.colorMode ==
                 pipetune_gtk::TrayIconColorMode::grayscale,
             "connecting icon must be grayscale") ||
      !check(connecting.badge ==
                 pipetune_gtk::StatusBadge::disconnected,
             "connecting icon badge differs")) {
    return 1;
  }

  const auto bypass = pipetune_gtk::statusIconPresentation(
      connectedState(pipetune::ProcessingMode::bypass));
  if (!check(bypass.colorMode ==
                 pipetune_gtk::TrayIconColorMode::grayscale,
             "bypass icon must be grayscale") ||
      !check(bypass.badge == pipetune_gtk::StatusBadge::none,
             "healthy bypass icon must not have a badge")) {
    return 1;
  }

  auto presetState = connectedState(pipetune::ProcessingMode::preset);
  const auto preset =
      pipetune_gtk::statusIconPresentation(presetState);
  if (!check(preset.colorMode ==
                 pipetune_gtk::TrayIconColorMode::color,
             "active preset icon must retain its colors") ||
      !check(preset.badge == pipetune_gtk::StatusBadge::none,
             "healthy preset icon must not have a badge")) {
    return 1;
  }

  presetState.runtime.defaultSinkActive = false;
  const auto attention =
      pipetune_gtk::statusIconPresentation(presetState);
  const auto valid =
      check(attention.colorMode ==
                pipetune_gtk::TrayIconColorMode::color,
            "attention icon must retain the preset color mode") &&
      check(attention.badge ==
                pipetune_gtk::StatusBadge::attention,
            "attention icon badge differs");
  return valid ? 0 : 1;
}
