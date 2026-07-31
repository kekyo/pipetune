#include "output-selection-model.h"

#include "localization.h"
#include "ui-message.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune_gtk {

static const pipetune::ControlOutputDevice *findOutput(
    const pipetune::ControlRuntimeStatus &runtime,
    std::string_view name) {
  for (const auto &output : runtime.availableOutputs) {
    if (output.name == name) {
      return &output;
    }
  }
  return nullptr;
}

static std::string deviceLabel(
    const pipetune::ControlOutputDevice &device) {
  auto label = device.description + " (" + device.name + ")";
  if (device.systemDefault) {
    label = formatUiMessage(localizedMessage(
        "{0} — system default", {label}));
  }
  return label;
}

static std::string effectiveOutputText(
    const pipetune::ControlRuntimeStatus &runtime) {
  if (runtime.selectedTarget.empty()) {
    return translate("Unavailable");
  }
  const auto *selected = findOutput(runtime, runtime.selectedTarget);
  if (selected == nullptr) {
    return runtime.selectedTarget;
  }
  return selected->description + " (" + selected->name + ")";
}

static std::string selectionReasonText(
    pipetune::ControlOutputSelectionReason reason) {
  switch (reason) {
  case pipetune::ControlOutputSelectionReason::unavailable:
    return translate("No audio output is available");
  case pipetune::ControlOutputSelectionReason::systemDefault:
    return translate("Following the system default");
  case pipetune::ControlOutputSelectionReason::preferred:
    return translate("Using the preferred output");
  case pipetune::ControlOutputSelectionReason::fallback:
    return translate(
        "Preferred output is unavailable — using the system default "
        "fallback");
  }
  return translate("Unknown");
}

OutputSelectionPresentation
makeOutputSelectionPresentation(const ApplicationState &state) {
  auto choices = std::vector<OutputDeviceChoice>{
      {.clearPreference = true,
       .target = {},
       .label = translate("System default"),
       .unavailable = false}};
  choices.reserve(state.runtime.availableOutputs.size() + 2);
  auto activeIndex = std::size_t{0};
  for (const auto &device : state.runtime.availableOutputs) {
    choices.push_back({
        .clearPreference = false,
        .target = device.name,
        .label = deviceLabel(device),
        .unavailable = false,
    });
    if (!state.runtime.preferredTarget.empty() &&
        state.runtime.preferredTarget == device.name) {
      activeIndex = choices.size() - 1;
    }
  }
  if (!state.runtime.preferredTarget.empty() &&
      findOutput(state.runtime, state.runtime.preferredTarget) == nullptr) {
    choices.push_back({
        .clearPreference = false,
        .target = state.runtime.preferredTarget,
        .label = formatUiMessage(localizedMessage(
            "Unavailable — {0}", {state.runtime.preferredTarget})),
        .unavailable = true,
    });
    activeIndex = choices.size() - 1;
  }
  return {
      .choices = std::move(choices),
      .activeIndex = activeIndex,
      .effectiveOutput = effectiveOutputText(state.runtime),
      .reason =
          selectionReasonText(state.runtime.outputSelectionReason),
      .sensitive =
          state.connection == ControlConnectionState::connected &&
          state.hasRuntimeStatus && !state.operationPending,
  };
}

} // namespace pipetune_gtk
