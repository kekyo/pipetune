#include "application-state.h"
#include "rate-selection-model.h"

#include "pipetune/sample_rate.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlFilterOutputStatus output(
    std::string name, std::string description,
    pipetune::SampleRateCapabilities capabilities,
    std::uint32_t dspRate, std::uint32_t outputRate,
    std::uint32_t activeRate, bool fallback) {
  return {
      .targetNodeName = std::move(name),
      .targetDescription = std::move(description),
      .filterNodeName = "pipetune.filter.test",
      .state = pipetune::ControlFilterState::active,
      .error = {},
      .channelCount = 2,
      .sampleRateCapabilities = std::move(capabilities),
      .dspSampleRate = dspRate,
      .outputSampleRate = outputRate,
      .activeOutputSampleRate = activeRate,
      .rateFallback = fallback,
      .latencyFrames = 64,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
  };
}

static pipetune_gtk::ApplicationState connectedState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  const auto usbCapabilities = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::range,
            .minimum = 44100,
            .maximum = 96000,
            .step = 0}}};
  const auto hdmiCapabilities = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 48000,
            .maximum = 48000,
            .step = 0},
           {.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 96000,
            .maximum = 96000,
            .step = 0}}};
  state.runtime.filterOutputs =
      {output("alsa_output.usb", "USB Audio", usbCapabilities, 192000,
              96000, 48000, true),
       output("alsa_output.hdmi", "HDMI", hdmiCapabilities, 192000,
              96000, 96000, true)};
  state.runtime.configuredRatePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  return state;
}

static bool testAggregateCapabilitiesAndEffectiveRates() {
  const auto state = connectedState();
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(
          state, state.runtime.configuredRatePolicy);
  return check(presentation.choices.size() == 6,
               "rate choices must contain Max plus five fixed rates") &&
         check(presentation.choices[0].label ==
                   "Max — highest supported rate per output",
               "Max rate choice label differs") &&
         check(presentation.choices[1].support ==
                       pipetune_gtk::DeviceRateSupport::unsupported &&
                   presentation.choices[1].label ==
                       "44.1 kHz — an output requires PipeWire resampling",
               "partial fixed-rate support must be explicit") &&
         check(presentation.choices[2].support ==
                       pipetune_gtk::DeviceRateSupport::supported &&
                   presentation.choices[2].label ==
                       "48 kHz — supported by all outputs",
               "all-output support must be explicit") &&
         check(presentation.activeRateIndex == 4 &&
                   presentation.activeEnforcementIndex == 0,
               "configured fixed-rate selection differs") &&
         check(presentation.effectiveRates.find(
                   "USB Audio: DSP 192 kHz → Output 96 kHz → Physical 48 kHz") !=
                   std::string::npos &&
                   presentation.effectiveRates.find(
                       "HDMI: DSP 192 kHz → Output 96 kHz → Physical 96 kHz") !=
                       std::string::npos &&
                   presentation.effectiveRates.find(
                       "PipeWire resampling") != std::string::npos,
               "per-output effective rates differ") &&
         check(presentation.sensitive,
               "healthy connected rate controls must be enabled");
}

static bool testUnknownCapabilitiesAndPendingState() {
  auto state = connectedState();
  state.runtime.filterOutputs[1].sampleRateCapabilities = {};
  const auto edited = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 384000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(state, edited);
  if (!check(presentation.choices[2].support ==
                     pipetune_gtk::DeviceRateSupport::unknown &&
                 presentation.choices[2].label ==
                     "48 kHz — support unknown",
             "unknown output support must be explicit") ||
      !check(presentation.activeRateIndex == 5 &&
                 presentation.activeEnforcementIndex == 1,
             "edited force policy selection differs")) {
    return false;
  }
  state.operationPending = true;
  presentation =
      pipetune_gtk::makeRateSelectionPresentation(state, edited);
  return check(!presentation.sensitive,
               "a pending operation must disable rate controls");
}

static bool testDisconnectedPresentation() {
  auto state = connectedState();
  pipetune_gtk::markControlDisconnected(state, "daemon stopped");
  state.hasRuntimeStatus = false;
  const auto presentation =
      pipetune_gtk::makeRateSelectionPresentation(
          state, pipetune::defaultSampleRatePolicy());
  return check(presentation.activeRateIndex == 0,
               "offline default rate choice must be Max") &&
         check(presentation.effectiveRates == "Rates unavailable",
               "offline effective rate summary differs") &&
         check(!presentation.sensitive,
               "disconnected rate controls must be disabled");
}

int main() {
  return testAggregateCapabilitiesAndEffectiveRates() &&
                 testUnknownCapabilitiesAndPendingState() &&
                 testDisconnectedPresentation()
             ? 0
             : 1;
}
