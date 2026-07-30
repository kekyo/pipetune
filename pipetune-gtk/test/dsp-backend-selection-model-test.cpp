#include "application-state.h"
#include "dsp-backend-selection-model.h"

#include "pipetune/dsp_backend.h"

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
  state.runtime.configuredDspBackend = pipetune::DspBackendKind::simd;
  state.runtime.effectiveDspBackend = pipetune::DspBackendKind::simd;
  state.runtime.dspBackendFallback = false;
  state.runtime.dspBackendError.clear();
  state.runtime.availableDspBackends = {{
      {.kind = pipetune::DspBackendKind::scalar,
       .available = true,
       .cpuRequirement = "none",
       .error = {}},
      {.kind = pipetune::DspBackendKind::simd,
       .available = true,
       .cpuRequirement = "x86-64-v2",
       .error = {}},
  }};
  return state;
}

static bool testAvailableBackends() {
  const auto state = connectedState();
  const auto presentation =
      pipetune_gtk::makeDspBackendSelectionPresentation(
          state, pipetune::DspBackendKind::simd);
  return check(presentation.choices.size() == 2,
               "DSP backend selector must contain scalar and SIMD") &&
         check(presentation.choices[0].kind ==
                       pipetune::DspBackendKind::scalar &&
                   presentation.choices[0].availabilityKnown &&
                   presentation.choices[0].available &&
                   presentation.choices[0].label.find("Scalar") !=
                       std::string::npos &&
                   presentation.choices[0].label.find("none") !=
                       std::string::npos,
               "available scalar presentation differs") &&
         check(presentation.choices[1].kind ==
                       pipetune::DspBackendKind::simd &&
                   presentation.choices[1].availabilityKnown &&
                   presentation.choices[1].available &&
                   presentation.choices[1].label.find("SIMD") !=
                       std::string::npos &&
                   presentation.choices[1].label.find("x86-64-v2") !=
                       std::string::npos,
               "available SIMD presentation differs") &&
         check(presentation.activeIndex == 1,
               "configured SIMD row must be active") &&
         check(presentation.effectiveBackend ==
                   "Configured SIMD  •  Effective SIMD",
               "healthy SIMD summary differs") &&
         check(presentation.selectedBackendAvailable,
               "available SIMD selection must be applicable") &&
         check(presentation.sensitive,
               "healthy connected DSP backend controls must be enabled");
}

static bool testFallbackAndUnavailableBackend() {
  auto state = connectedState();
  state.runtime.effectiveDspBackend = pipetune::DspBackendKind::scalar;
  state.runtime.dspBackendFallback = true;
  state.runtime.dspBackendError = "required CPU features are unavailable";
  state.runtime.availableDspBackends[1].available = false;
  state.runtime.availableDspBackends[1].error =
      "required CPU features are unavailable";
  const auto unavailable =
      pipetune_gtk::makeDspBackendSelectionPresentation(
          state, pipetune::DspBackendKind::simd);
  if (!check(!unavailable.choices[1].available &&
                 unavailable.choices[1].label.find("unavailable") !=
                     std::string::npos &&
                 unavailable.choices[1].label.find(
                     "required CPU features") != std::string::npos,
             "unavailable SIMD row must include its diagnostic") ||
      !check(!unavailable.selectedBackendAvailable,
             "unavailable selected backend must not be applicable") ||
      !check(unavailable.effectiveBackend ==
                 "Configured SIMD  •  Effective Scalar  •  "
                 "Fallback — required CPU features are unavailable",
             "SIMD fallback summary differs")) {
    return false;
  }

  const auto scalar =
      pipetune_gtk::makeDspBackendSelectionPresentation(
          state, pipetune::DspBackendKind::scalar);
  return check(scalar.activeIndex == 0 &&
                   scalar.selectedBackendAvailable,
               "fallback state must allow selecting scalar");
}

static bool testUnavailableAndDisconnectedState() {
  auto state = connectedState();
  state.runtime.configuredDspBackend =
      pipetune::DspBackendKind::scalar;
  state.runtime.effectiveDspBackend.reset();
  state.runtime.dspBackendError = "scalar backend could not be loaded";
  state.runtime.availableDspBackends[0].available = false;
  state.runtime.availableDspBackends[0].error =
      "scalar backend could not be loaded";
  const auto unavailable =
      pipetune_gtk::makeDspBackendSelectionPresentation(
          state, pipetune::DspBackendKind::scalar);
  if (!check(unavailable.effectiveBackend ==
                 "Configured Scalar  •  Effective unavailable — "
                 "scalar backend could not be loaded",
             "missing scalar summary differs") ||
      !check(!unavailable.selectedBackendAvailable,
             "missing scalar backend must not be applicable")) {
    return false;
  }

  pipetune_gtk::markControlDisconnected(state, "daemon stopped");
  state.hasRuntimeStatus = false;
  const auto disconnected =
      pipetune_gtk::makeDspBackendSelectionPresentation(
          state, pipetune::DspBackendKind::simd);
  return check(!disconnected.choices[0].availabilityKnown &&
                   !disconnected.choices[1].availabilityKnown,
               "offline backend availability must be unknown") &&
         check(disconnected.activeIndex == 1,
               "offline edited backend row differs") &&
         check(disconnected.effectiveBackend ==
                   "Effective backend unavailable",
               "offline backend summary differs") &&
         check(!disconnected.sensitive,
               "disconnected live backend controls must be disabled");
}

static bool testTransitionAndPendingState() {
  auto state = connectedState();
  state.runtime.rateTransitioning = true;
  auto presentation =
      pipetune_gtk::makeDspBackendSelectionPresentation(
          state, pipetune::DspBackendKind::simd);
  if (!check(!presentation.sensitive,
             "PCM rate transition must disable backend switching")) {
    return false;
  }
  state.runtime.rateTransitioning = false;
  state.operationPending = true;
  presentation = pipetune_gtk::makeDspBackendSelectionPresentation(
      state, pipetune::DspBackendKind::simd);
  return check(!presentation.sensitive,
               "another pending operation must disable backend switching");
}

int main() {
  return testAvailableBackends() &&
                 testFallbackAndUnavailableBackend() &&
                 testUnavailableAndDisconnectedState() &&
                 testTransitionAndPendingState()
             ? 0
             : 1;
}
