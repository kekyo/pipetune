#include "dsp-idle-selection-model.h"

#include "localization.h"
#include "ui-message.h"

#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

static std::string_view policyDisplayName(
    pipetune::DspIdlePolicy policy) {
  switch (policy) {
  case pipetune::DspIdlePolicy::conservative:
    return translate("Conservative");
  case pipetune::DspIdlePolicy::exact:
    return translate("Exact");
  }
  return translate("Unknown");
}

static std::string_view stateDisplayName(
    pipetune::DspIdleState state, bool pipeWireIdle) {
  switch (state) {
  case pipetune::DspIdleState::active:
    return pipeWireIdle ? translate("Paused") : translate("Active");
  case pipetune::DspIdleState::draining:
    return pipeWireIdle ? translate("Paused") : translate("Draining");
  case pipetune::DspIdleState::sleeping:
    return translate("Sleeping");
  }
  return translate("Unknown");
}

static std::string runtimeStatusText(const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return translate("DSP idle state unavailable");
  }

  const auto *statusTemplate =
      state.runtime.pipeWireIdle
          ? translate(
                "Configured {0}  •  DSP {1}  •  Skipped {2} frames  •  "
                "Sleep transitions {3}  •  PipeWire paused")
          : translate(
                "Configured {0}  •  DSP {1}  •  Skipped {2} frames  •  "
                "Sleep transitions {3}  •  PipeWire running");
  return formatUiMessage(
      {.translatable = false,
       .messageId = statusTemplate,
       .arguments =
           {std::string(policyDisplayName(state.runtime.dspIdlePolicy)),
            std::string(stateDisplayName(state.runtime.dspIdleState,
                                         state.runtime.pipeWireIdle)),
            std::to_string(state.runtime.dspIdleSkippedFrames),
            std::to_string(state.runtime.dspIdleSleepTransitions)}});
}

DspIdleSelectionPresentation makeDspIdleSelectionPresentation(
    const ApplicationState &state,
    pipetune::DspIdlePolicy editedPolicy) {
  auto choices = std::vector<DspIdleChoice>{
      {.policy = pipetune::DspIdlePolicy::conservative,
       .label = translate(
           "Conservative — sleep after exact-zero input and inaudible tails")},
      {.policy = pipetune::DspIdlePolicy::exact,
       .label = translate(
           "Exact — require exact-zero input and output")}};
  const auto activeIndex =
      editedPolicy == pipetune::DspIdlePolicy::exact
          ? std::size_t{1}
          : std::size_t{0};
  return {
      .choices = std::move(choices),
      .activeIndex = activeIndex,
      .runtimeStatus = runtimeStatusText(state),
      .sensitive =
          state.connection == ControlConnectionState::connected &&
          state.hasRuntimeStatus && !state.operationPending,
  };
}

} // namespace pipetune_gtk
