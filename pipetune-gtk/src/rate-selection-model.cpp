#include "rate-selection-model.h"

#include "localization.h"
#include "ui-message.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune_gtk {

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
    const pipetune::ControlRuntimeStatus &runtime,
    std::uint32_t sampleRate) {
  auto foundOutput = false;
  auto foundUnknown = false;
  for (const auto &output : runtime.filterOutputs) {
    foundOutput = true;
    if (!output.sampleRateCapabilities.known) {
      foundUnknown = true;
      continue;
    }
    if (!pipetune::sampleRateCapabilitiesSupport(
            output.sampleRateCapabilities, sampleRate)) {
      return DeviceRateSupport::unsupported;
    }
  }
  if (!foundOutput || foundUnknown) {
    return DeviceRateSupport::unknown;
  }
  return DeviceRateSupport::supported;
}

static std::string fixedRateLabel(std::uint32_t sampleRate,
                                  DeviceRateSupport support) {
  auto label = sampleRateText(sampleRate);
  switch (support) {
  case DeviceRateSupport::supported:
    return formatUiMessage(localizedMessage(
        "{0} — supported by all outputs", {label}));
  case DeviceRateSupport::unsupported:
    return formatUiMessage(localizedMessage(
        "{0} — an output requires PipeWire resampling", {label}));
  case DeviceRateSupport::unknown:
    return formatUiMessage(localizedMessage(
        "{0} — support unknown", {label}));
  case DeviceRateSupport::notApplicable:
    break;
  }
  return label;
}

static std::string filterStateText(
    pipetune::ControlFilterState state) {
  switch (state) {
  case pipetune::ControlFilterState::waiting:
    return translate("waiting");
  case pipetune::ControlFilterState::active:
    return translate("active");
  case pipetune::ControlFilterState::bypassed:
    return translate("bypassed");
  case pipetune::ControlFilterState::error:
    return translate("error");
  }
  return translate("unknown");
}

static std::string outputName(
    const pipetune::ControlFilterOutputStatus &output) {
  return output.targetDescription.empty() ? output.targetNodeName
                                          : output.targetDescription;
}

static std::string effectiveOutputRateText(
    const pipetune::ControlFilterOutputStatus &output) {
  auto text = outputName(output) + ": ";
  if (output.state != pipetune::ControlFilterState::active) {
    text += filterStateText(output.state);
    if (!output.error.empty()) {
      text += " — " + output.error;
    }
    return text;
  }

  const auto physical =
      output.activeOutputSampleRate == 0
          ? std::string(translate("idle"))
          : sampleRateText(output.activeOutputSampleRate);
  text += formatUiMessage(localizedMessage(
      "DSP {0} → Output {1} → Physical {2}",
      {sampleRateText(output.dspSampleRate),
       sampleRateText(output.outputSampleRate), physical}));
  if (output.rateFallback ||
      (output.dspSampleRate != 0 && output.outputSampleRate != 0 &&
       output.dspSampleRate != output.outputSampleRate)) {
    text += "  •  ";
    text += translate("PipeWire resampling");
  }
  return text;
}

static std::string effectiveRateText(const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return translate("Rates unavailable");
  }
  if (state.runtime.filterOutputs.empty()) {
    return translate("No physical outputs");
  }
  auto text = std::string{};
  for (const auto &output : state.runtime.filterOutputs) {
    if (!text.empty()) {
      text += "  |  ";
    }
    text += effectiveOutputRateText(output);
  }
  return text;
}

RateSelectionPresentation makeRateSelectionPresentation(
    const ApplicationState &state,
    const pipetune::SampleRatePolicy &editedPolicy) {
  auto choices = std::vector<SampleRateChoice>{
      {.mode = pipetune::SampleRateMode::maximum,
       .fixedRate = 0,
       .label = translate("Max — highest supported rate per output"),
       .support = DeviceRateSupport::notApplicable}};
  choices.reserve(pipetune::selectableSampleRates().size() + 1);
  auto activeRateIndex = std::size_t{0};
  for (const auto sampleRate : pipetune::selectableSampleRates()) {
    const auto support = fixedRateSupport(state.runtime, sampleRate);
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
          state.hasRuntimeStatus && !state.operationPending,
  };
}

} // namespace pipetune_gtk
