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
  state.runtime.filterOutputs = {{
      .targetNodeName = "alsa_output.speaker",
      .targetDescription = "Speakers",
      .filterNodeName = "pipetune.filter.speaker",
      .state = pipetune::ControlFilterState::active,
      .error = {},
      .channelCount = 2,
      .sampleRateCapabilities = {},
      .dspSampleRate = 48000,
      .outputSampleRate = 48000,
      .activeOutputSampleRate = 48000,
      .rateFallback = false,
      .latencyFrames = 64,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
  }};
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

  presetState.runtime.filterOutputs[0].state =
      pipetune::ControlFilterState::bypassed;
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
