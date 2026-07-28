#include "application-state.h"
#include "output-selection-model.h"

#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune_gtk::ApplicationState connectedState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime.preferredTarget.clear();
  state.runtime.selectedTarget = "alsa_output.speaker";
  state.runtime.outputSelectionReason =
      pipetune::ControlOutputSelectionReason::systemDefault;
  state.runtime.availableOutputs = {
      {.name = "alsa_output.speaker",
       .description = "Built-in Speakers",
       .systemDefault = true,
       .preferred = false,
       .selected = true},
      {.name = "alsa_output.headphones",
       .description = "USB Headphones",
       .systemDefault = false,
       .preferred = false,
       .selected = false}};
  return state;
}

static bool testSystemDefaultPresentation() {
  const auto presentation =
      pipetune_gtk::makeOutputSelectionPresentation(connectedState());
  return check(presentation.choices.size() == 3,
               "system-default presentation must contain clear plus devices") &&
         check(presentation.choices[0].clearPreference &&
                   presentation.choices[0].target.empty() &&
                   presentation.choices[0].label == "System default",
               "first output choice must clear the preference") &&
         check(presentation.activeIndex == 0,
               "system-default mode must activate the first choice") &&
         check(presentation.effectiveOutput.find("Built-in Speakers") !=
                   std::string::npos,
               "effective output must use the engine-selected description") &&
         check(presentation.reason.find("system default") !=
                   std::string::npos,
               "system-default reason text differs") &&
         check(presentation.sensitive,
               "connected output selection must be enabled");
}

static bool testPreferredAndFallbackPresentations() {
  auto preferredState = connectedState();
  preferredState.runtime.preferredTarget = "alsa_output.headphones";
  preferredState.runtime.selectedTarget = "alsa_output.headphones";
  preferredState.runtime.outputSelectionReason =
      pipetune::ControlOutputSelectionReason::preferred;
  preferredState.runtime.availableOutputs[0].selected = false;
  preferredState.runtime.availableOutputs[1].preferred = true;
  preferredState.runtime.availableOutputs[1].selected = true;
  const auto preferred =
      pipetune_gtk::makeOutputSelectionPresentation(preferredState);
  if (!check(preferred.activeIndex == 2,
             "available preference must activate its device") ||
      !check(preferred.effectiveOutput.find("USB Headphones") !=
                 std::string::npos,
             "preferred effective output text differs") ||
      !check(preferred.reason.find("preferred output") !=
                 std::string::npos,
             "preferred reason text differs")) {
    return false;
  }

  auto fallbackState = connectedState();
  fallbackState.runtime.preferredTarget =
      "alsa_output.disconnected-usb";
  fallbackState.runtime.outputSelectionReason =
      pipetune::ControlOutputSelectionReason::fallback;
  const auto fallback =
      pipetune_gtk::makeOutputSelectionPresentation(fallbackState);
  if (!check(fallback.choices.size() == 4,
             "missing preference must add one unavailable choice") ||
      !check(fallback.activeIndex == 3 &&
                 fallback.choices[3].unavailable &&
                 fallback.choices[3].target ==
                     "alsa_output.disconnected-usb",
             "unavailable preference choice differs") ||
      !check(fallback.effectiveOutput.find("Built-in Speakers") !=
                 std::string::npos,
             "fallback must display the engine-selected output") ||
      !check(fallback.reason.find("unavailable") !=
                 std::string::npos,
             "fallback reason must explain the missing preference")) {
    return false;
  }

  pipetune_gtk::markControlDisconnected(fallbackState, "daemon stopped");
  const auto disconnected =
      pipetune_gtk::makeOutputSelectionPresentation(fallbackState);
  return check(disconnected.choices.size() == fallback.choices.size() &&
                   disconnected.activeIndex == fallback.activeIndex,
               "disconnection must retain the last output choices") &&
         check(!disconnected.sensitive,
               "disconnected output selection must be disabled");
}

static bool testUnavailableAndPendingPresentations() {
  auto unavailableState = connectedState();
  unavailableState.runtime.preferredTarget =
      "alsa_output.disconnected-usb";
  unavailableState.runtime.selectedTarget.clear();
  unavailableState.runtime.outputSelectionReason =
      pipetune::ControlOutputSelectionReason::unavailable;
  unavailableState.runtime.availableOutputs.clear();
  const auto unavailable =
      pipetune_gtk::makeOutputSelectionPresentation(unavailableState);
  if (!check(unavailable.choices.size() == 2 &&
                 unavailable.activeIndex == 1 &&
                 unavailable.choices[1].unavailable,
             "no-device state must retain an unavailable preference") ||
      !check(unavailable.effectiveOutput == "Unavailable",
             "no-device effective output text differs") ||
      !check(unavailable.reason.find("No audio output") !=
                 std::string::npos,
             "no-device reason text differs")) {
    return false;
  }

  auto pendingState = connectedState();
  pipetune_gtk::setControlOperationPending(pendingState, true);
  const auto pending =
      pipetune_gtk::makeOutputSelectionPresentation(pendingState);
  return check(!pending.sensitive,
               "pending control operation must disable output selection");
}

int main() {
  return testSystemDefaultPresentation() &&
                 testPreferredAndFallbackPresentations() &&
                 testUnavailableAndPendingPresentations()
             ? 0
             : 1;
}
