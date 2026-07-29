#include "rate-selection-model.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune_gtk {

static const pipetune::SampleRateCapabilities *selectedCapabilities(
    const pipetune::ControlRuntimeStatus &runtime) {
  for (const auto &output : runtime.availableOutputs) {
    if (output.name == runtime.selectedTarget) {
      return &output.sampleRateCapabilities;
    }
  }
  return nullptr;
}

static std::string sampleRateText(std::uint32_t sampleRate) {
  if (sampleRate == 0) {
    return "unavailable";
  }
  if (sampleRate % 1000 == 0) {
    return std::to_string(sampleRate / 1000) + " kHz";
  }
  const auto whole = sampleRate / 1000;
  const auto decimal = (sampleRate % 1000) / 100;
  return std::to_string(whole) + "." + std::to_string(decimal) + " kHz";
}

static DeviceRateSupport fixedRateSupport(
    const pipetune::SampleRateCapabilities *capabilities,
    std::uint32_t sampleRate) {
  if (capabilities == nullptr || !capabilities->known) {
    return DeviceRateSupport::unknown;
  }
  return pipetune::sampleRateCapabilitiesSupport(*capabilities, sampleRate)
             ? DeviceRateSupport::supported
             : DeviceRateSupport::unsupported;
}

static std::string fixedRateLabel(std::uint32_t sampleRate,
                                  DeviceRateSupport support) {
  auto label = sampleRateText(sampleRate);
  switch (support) {
  case DeviceRateSupport::supported:
    return label + " — supported";
  case DeviceRateSupport::unsupported:
    return label + " — unsupported; PipeWire will resample";
  case DeviceRateSupport::unknown:
    return label + " — support unknown";
  case DeviceRateSupport::notApplicable:
    break;
  }
  return label;
}

static std::string effectiveRateText(
    const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return "Rates unavailable";
  }
  const auto &runtime = state.runtime;
  auto text = std::string{};
  if (runtime.rateTransitioning) {
    text = "Switching — ";
  }
  text += "Input/DSP " + sampleRateText(runtime.dspSampleRate);
  text += "  •  Output " +
          sampleRateText(runtime.selectedOutputSampleRate);
  text += "  •  Physical ";
  text += runtime.activeOutputSampleRate == 0
              ? std::string("idle")
              : sampleRateText(runtime.activeOutputSampleRate);
  if (runtime.rateFallback ||
      (runtime.dspSampleRate != 0 &&
       runtime.selectedOutputSampleRate != 0 &&
       runtime.dspSampleRate != runtime.selectedOutputSampleRate)) {
    text += "  •  PipeWire resampling";
  }
  return text;
}

RateSelectionPresentation makeRateSelectionPresentation(
    const ApplicationState &state,
    const pipetune::SampleRatePolicy &editedPolicy) {
  const auto *capabilities = selectedCapabilities(state.runtime);
  auto choices = std::vector<SampleRateChoice>{{
      .mode = pipetune::SampleRateMode::maximum,
      .fixedRate = 0,
      .label = "Max — highest supported rate",
      .support = DeviceRateSupport::notApplicable,
  }};
  choices.reserve(pipetune::selectableSampleRates().size() + 1);
  auto activeRateIndex = std::size_t{0};
  for (const auto sampleRate : pipetune::selectableSampleRates()) {
    const auto support = fixedRateSupport(capabilities, sampleRate);
    choices.push_back({
        .mode = pipetune::SampleRateMode::fixed,
        .fixedRate = sampleRate,
        .label = fixedRateLabel(sampleRate, support),
        .support = support,
    });
    if (editedPolicy.mode == pipetune::SampleRateMode::fixed &&
        editedPolicy.fixedRate == sampleRate) {
      activeRateIndex = choices.size() - 1;
    }
  }

  return {
      .choices = std::move(choices),
      .activeRateIndex = activeRateIndex,
      .activeEnforcementIndex =
          editedPolicy.enforcement ==
                  pipetune::SampleRateEnforcement::force
              ? std::size_t{1}
              : std::size_t{0},
      .effectiveRates = effectiveRateText(state),
      .sensitive =
          state.connection == ControlConnectionState::connected &&
          state.hasRuntimeStatus && !state.operationPending &&
          !state.runtime.rateTransitioning,
  };
}

} // namespace pipetune_gtk
