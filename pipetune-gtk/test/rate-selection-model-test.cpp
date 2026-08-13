/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "application-state.h"
#include "rate-selection-model.h"

#include "pipetune/sample_rate.h"

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
  state.runtime.configuredRatePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  state.runtime.dspSampleRate = 192000;
  state.runtime.graphSampleRate = 192000;
  return state;
}

static bool testChoicesAndEffectiveRates() {
  const auto state = connectedState();
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(
          state, state.runtime.configuredRatePolicy);
  return check(presentation.choices.size() == 6,
               "rate choices must contain Automatic plus five fixed rates") &&
         check(presentation.choices[0].label ==
                   "Automatic — follow PipeWire graph sampling frequency",
               "Automatic rate choice label differs") &&
         check(presentation.choices[1].label == "44.1 kHz",
               "44.1 kHz fixed-rate label differs") &&
         check(presentation.choices[3].label == "96 kHz",
               "96 kHz fixed-rate label differs") &&
         check(presentation.choices[4].label == "192 kHz",
               "192 kHz fixed-rate label differs") &&
         check(presentation.choices[5].label == "384 kHz",
               "384 kHz fixed-rate label differs") &&
         check(presentation.activeRateIndex == 4,
               "configured fixed-rate row differs") &&
         check(presentation.activeEnforcementIndex == 0,
               "suggest enforcement row differs") &&
         check(presentation.enforcementSensitive,
               "fixed-rate enforcement must be editable") &&
         check(presentation.effectiveRates ==
                   "DSP sampling frequency 192 kHz  •  "
                   "PipeWire graph sampling frequency 192 kHz",
               "effective rate summary differs") &&
         check(presentation.sensitive,
               "healthy connected rate controls must be enabled");
}

static bool testTransition() {
  auto state = connectedState();
  state.runtime.rateTransitioning = true;
  const auto edited = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 384000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(state, edited);
  return check(presentation.choices[5].label == "384 kHz",
               "fixed-rate label must be device independent") &&
         check(presentation.activeRateIndex == 5 &&
                   presentation.activeEnforcementIndex == 1,
               "edited force policy selection differs") &&
         check(presentation.enforcementSensitive,
               "fixed force policy must expose enforcement") &&
         check(!presentation.sensitive,
               "automatic rate transition must disable rate controls") &&
         check(presentation.effectiveRates.starts_with("Switching — "),
               "transitioning rate summary must be explicit");
}

static bool testDisconnectedPresentation() {
  auto state = connectedState();
  pipetune_gtk::markControlDisconnected(state, "daemon stopped");
  state.hasRuntimeStatus = false;
  const auto policy = pipetune::defaultSampleRatePolicy();
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(state, policy);
  return check(presentation.activeRateIndex == 0,
               "offline default rate choice must be Automatic") &&
         check(!presentation.enforcementSensitive,
               "automatic mode must not expose fixed enforcement") &&
         check(presentation.effectiveRates ==
                   "Sampling frequencies unavailable",
               "offline effective rate summary differs") &&
         check(!presentation.sensitive,
               "disconnected live rate controls must be disabled");
}

int main() {
  return testChoicesAndEffectiveRates() && testTransition() &&
                 testDisconnectedPresentation()
             ? 0
             : 1;
}
