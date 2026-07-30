#include "application-state.h"
#include "dsp-idle-selection-model.h"

#include "pipetune/dsp_idle.h"

#include <iostream>
#include <string>
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
  state.runtime.dspIdlePolicy = pipetune::DspIdlePolicy::exact;
  state.runtime.dspIdleState = pipetune::DspIdleState::sleeping;
  state.runtime.dspIdleSkippedFrames = 48000;
  state.runtime.dspIdleSleepTransitions = 2;
  state.runtime.pipeWireIdle = true;
  return state;
}

static bool testConnectedPresentation() {
  const auto state = connectedState();
  const auto presentation =
      pipetune_gtk::makeDspIdleSelectionPresentation(
          state, pipetune::DspIdlePolicy::conservative);
  return check(presentation.choices.size() == 2,
               "DSP idle selector must contain both policies") &&
         check(presentation.choices[0].policy ==
                       pipetune::DspIdlePolicy::conservative &&
                   presentation.choices[0].label.find("Conservative") !=
                       std::string::npos &&
                   presentation.choices[0].label.find("tails") !=
                       std::string::npos,
               "conservative DSP idle choice differs") &&
         check(presentation.choices[1].policy ==
                       pipetune::DspIdlePolicy::exact &&
                   presentation.choices[1].label.find("Exact") !=
                       std::string::npos &&
                   presentation.choices[1].label.find("exact-zero") !=
                       std::string::npos,
               "exact DSP idle choice differs") &&
         check(presentation.activeIndex == 0,
               "edited DSP idle policy must select its row") &&
         check(presentation.runtimeStatus ==
                   "Configured Exact  •  DSP Sleeping  •  Skipped 48000 "
                   "frames  •  Sleep transitions 2  •  PipeWire paused",
               "sleeping DSP idle status differs") &&
         check(presentation.sensitive,
               "connected DSP idle selector must be enabled");
}

static bool testRuntimeStatesAndSensitivity() {
  auto state = connectedState();
  state.runtime.dspIdleState = pipetune::DspIdleState::active;
  auto presentation =
      pipetune_gtk::makeDspIdleSelectionPresentation(
          state, pipetune::DspIdlePolicy::exact);
  if (!check(presentation.runtimeStatus.find("DSP Paused") !=
                 std::string::npos &&
                 presentation.runtimeStatus.find("DSP Active") ==
                     std::string::npos,
             "PipeWire-paused DSP status must show effective inactivity")) {
    return false;
  }

  state.runtime.dspIdleState = pipetune::DspIdleState::draining;
  state.runtime.pipeWireIdle = false;
  presentation =
      pipetune_gtk::makeDspIdleSelectionPresentation(
          state, pipetune::DspIdlePolicy::exact);
  if (!check(presentation.activeIndex == 1,
             "exact DSP idle policy must select its row") ||
      !check(presentation.runtimeStatus.find("DSP Draining") !=
                 std::string::npos &&
                 presentation.runtimeStatus.find("PipeWire running") !=
                     std::string::npos,
             "draining DSP idle status differs")) {
    return false;
  }

  state.operationPending = true;
  presentation = pipetune_gtk::makeDspIdleSelectionPresentation(
      state, pipetune::DspIdlePolicy::exact);
  if (!check(!presentation.sensitive,
             "pending operation must disable DSP idle selection")) {
    return false;
  }

  pipetune_gtk::markControlDisconnected(state, "daemon stopped");
  state.hasRuntimeStatus = false;
  presentation = pipetune_gtk::makeDspIdleSelectionPresentation(
      state, pipetune::DspIdlePolicy::exact);
  return check(presentation.activeIndex == 1,
               "offline edited DSP idle policy differs") &&
         check(presentation.runtimeStatus ==
                   "DSP idle state unavailable",
               "offline DSP idle status differs") &&
         check(!presentation.sensitive,
               "offline DSP idle selector must not be live-sensitive");
}

int main() {
  return testConnectedPresentation() &&
                 testRuntimeStatesAndSensitivity()
             ? 0
             : 1;
}
