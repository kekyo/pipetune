#include "rate-selection-model.h"

#include "localization.h"
#include "ui-message.h"

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
    return translate("unavailable");
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
    return formatUiMessage(localizedMessage(
        "{0} — supported", {label}));
  case DeviceRateSupport::unsupported:
    return formatUiMessage(localizedMessage(
        "{0} — unsupported; PipeWire will resample", {label}));
  case DeviceRateSupport::unknown:
    return formatUiMessage(localizedMessage(
        "{0} — support unknown", {label}));
  case DeviceRateSupport::notApplicable:
    break;
  }
  return label;
}

static std::string effectiveRateText(
    const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return translate("Rates unavailable");
  }
  const auto &runtime = state.runtime;
  const auto physical =
      runtime.activeOutputSampleRate == 0
          ? std::string(translate("idle"))
          : sampleRateText(runtime.activeOutputSampleRate);
  auto text = formatUiMessage(localizedMessage(
      "Input/DSP {0}  •  Output {1}  •  Physical {2}",
      {sampleRateText(runtime.dspSampleRate),
       sampleRateText(runtime.selectedOutputSampleRate), physical}));
  if (runtime.rateTransitioning) {
    text = formatUiMessage(localizedMessage(
        "Switching — {0}", {text}));
  }
  if (runtime.rateFallback ||
      (runtime.dspSampleRate != 0 &&
       runtime.selectedOutputSampleRate != 0 &&
       runtime.dspSampleRate != runtime.selectedOutputSampleRate)) {
    text = formatUiMessage(localizedMessage(
        "{0}  •  PipeWire resampling", {text}));
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
      .label = translate("Max — highest supported rate"),
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
