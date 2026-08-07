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

static std::string effectiveRateText(
    const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return translate("Sampling frequencies unavailable");
  }
  const auto &runtime = state.runtime;
  auto text = formatUiMessage(localizedMessage(
      "DSP sampling frequency {0}  •  "
      "PipeWire graph sampling frequency {1}",
      {sampleRateText(runtime.dspSampleRate),
       sampleRateText(runtime.graphSampleRate)}));
  if (runtime.rateTransitioning) {
    text = formatUiMessage(localizedMessage(
        "Switching — {0}", {text}));
  }
  return text;
}

RateSelectionPresentation makeRateSelectionPresentation(
    const ApplicationState &state,
    const pipetune::SampleRatePolicy &editedPolicy) {
  auto choices = std::vector<SampleRateChoice>{{
      .mode = pipetune::SampleRateMode::automatic,
      .fixedRate = 0,
      .label = translate(
          "Automatic — follow PipeWire graph sampling frequency"),
  }};
  choices.reserve(pipetune::selectableSampleRates().size() + 1);
  auto activeRateIndex = std::size_t{0};
  for (const auto sampleRate : pipetune::selectableSampleRates()) {
    choices.push_back({
        .mode = pipetune::SampleRateMode::fixed,
        .fixedRate = sampleRate,
        .label = sampleRateText(sampleRate),
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
      .enforcementSensitive =
          editedPolicy.mode == pipetune::SampleRateMode::fixed,
      .effectiveRates = effectiveRateText(state),
      .sensitive =
          state.connection == ControlConnectionState::connected &&
          state.hasRuntimeStatus && !state.operationPending &&
          !state.runtime.rateTransitioning,
  };
}

} // namespace pipetune_gtk
