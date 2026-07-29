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
  state.runtime.selectedTarget = "alsa_output.usb";
  state.runtime.availableOutputs = {
      {.name = "alsa_output.usb",
       .description = "USB Audio",
       .systemDefault = true,
       .preferred = false,
       .selected = true,
       .sampleRateCapabilities =
           {.known = true,
            .constraints =
                {{.kind = pipetune::SampleRateConstraintKind::discrete,
                  .minimum = 44100,
                  .maximum = 44100,
                  .step = 0},
                 {.kind = pipetune::SampleRateConstraintKind::discrete,
                  .minimum = 48000,
                  .maximum = 48000,
                  .step = 0},
                 {.kind = pipetune::SampleRateConstraintKind::discrete,
                  .minimum = 96000,
                  .maximum = 96000,
                  .step = 0}}}}};
  state.runtime.configuredRatePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  state.runtime.dspSampleRate = 192000;
  state.runtime.selectedOutputSampleRate = 96000;
  state.runtime.activeOutputSampleRate = 48000;
  state.runtime.rateFallback = true;
  return state;
}

static bool testKnownCapabilitiesAndEffectiveRates() {
  const auto state = connectedState();
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(
          state, state.runtime.configuredRatePolicy);
  return check(presentation.choices.size() == 6,
               "rate choices must contain Max plus five fixed rates") &&
         check(presentation.choices[0].label ==
                   "Max — highest supported rate",
               "Max rate choice label differs") &&
         check(presentation.choices[1].label ==
                   "44.1 kHz — supported",
               "supported fixed-rate label differs") &&
         check(presentation.choices[3].label ==
                   "96 kHz — supported",
               "supported high-rate label differs") &&
         check(presentation.choices[4].label ==
                   "192 kHz — unsupported; PipeWire will resample",
               "unsupported fixed-rate label differs") &&
         check(presentation.choices[5].label ==
                   "384 kHz — unsupported; PipeWire will resample",
               "unsupported maximum fixed-rate label differs") &&
         check(presentation.activeRateIndex == 4,
               "configured fixed-rate row differs") &&
         check(presentation.activeEnforcementIndex == 0,
               "suggest enforcement row differs") &&
         check(presentation.effectiveRates ==
                   "Input/DSP 192 kHz  •  Output 96 kHz  •  "
                   "Physical 48 kHz  •  PipeWire resampling",
               "effective rate summary differs") &&
         check(presentation.sensitive,
               "healthy connected rate controls must be enabled");
}

static bool testUnknownCapabilitiesAndTransition() {
  auto state = connectedState();
  state.runtime.availableOutputs[0].sampleRateCapabilities = {};
  state.runtime.rateTransitioning = true;
  const auto edited = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 384000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(state, edited);
  return check(presentation.choices[5].label ==
                   "384 kHz — support unknown",
               "unknown device support must be explicit") &&
         check(presentation.activeRateIndex == 5 &&
                   presentation.activeEnforcementIndex == 1,
               "edited force policy selection differs") &&
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
               "offline default rate choice must be Max") &&
         check(presentation.effectiveRates == "Rates unavailable",
               "offline effective rate summary differs") &&
         check(!presentation.sensitive,
               "disconnected live rate controls must be disabled");
}

int main() {
  return testKnownCapabilitiesAndEffectiveRates() &&
                 testUnknownCapabilitiesAndTransition() &&
                 testDisconnectedPresentation()
             ? 0
             : 1;
}
