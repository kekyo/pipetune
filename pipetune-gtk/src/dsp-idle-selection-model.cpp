#include "dsp-idle-selection-model.h"

#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

static std::string_view policyDisplayName(
    pipetune::DspIdlePolicy policy) {
  switch (policy) {
  case pipetune::DspIdlePolicy::conservative:
    return "Conservative";
  case pipetune::DspIdlePolicy::exact:
    return "Exact";
  }
  return "Unknown";
}

static std::string_view stateDisplayName(
    pipetune::DspIdleState state, bool pipeWireIdle) {
  switch (state) {
  case pipetune::DspIdleState::active:
    return pipeWireIdle ? "Paused" : "Active";
  case pipetune::DspIdleState::draining:
    return pipeWireIdle ? "Paused" : "Draining";
  case pipetune::DspIdleState::sleeping:
    return "Sleeping";
  }
  return "Unknown";
}

static std::string runtimeStatusText(const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return "DSP idle state unavailable";
  }

  auto text = std::string("Configured ");
  text += policyDisplayName(state.runtime.dspIdlePolicy);
  text += "  •  DSP ";
  text += stateDisplayName(state.runtime.dspIdleState,
                           state.runtime.pipeWireIdle);
  text += "  •  Skipped ";
  text += std::to_string(state.runtime.dspIdleSkippedFrames);
  text += " frames  •  Sleep transitions ";
  text += std::to_string(state.runtime.dspIdleSleepTransitions);
  text += state.runtime.pipeWireIdle ? "  •  PipeWire paused"
                                     : "  •  PipeWire running";
  return text;
}

DspIdleSelectionPresentation makeDspIdleSelectionPresentation(
    const ApplicationState &state,
    pipetune::DspIdlePolicy editedPolicy) {
  auto choices = std::vector<DspIdleChoice>{
      {.policy = pipetune::DspIdlePolicy::conservative,
       .label =
           "Conservative — sleep after exact-zero input and inaudible tails"},
      {.policy = pipetune::DspIdlePolicy::exact,
       .label = "Exact — require exact-zero input and output"}};
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
