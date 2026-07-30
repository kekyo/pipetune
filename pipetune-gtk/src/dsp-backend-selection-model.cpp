#include "dsp-backend-selection-model.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune_gtk {

static std::string_view backendDisplayName(
    pipetune::DspBackendKind kind) {
  return kind == pipetune::DspBackendKind::scalar
             ? std::string_view("Scalar")
             : std::string_view("SIMD");
}

static const pipetune::ControlDspBackendAvailability *
backendAvailability(const pipetune::ControlRuntimeStatus &status,
                    pipetune::DspBackendKind kind) {
  for (const auto &availability : status.availableDspBackends) {
    if (availability.kind == kind) {
      return &availability;
    }
  }
  return nullptr;
}

static DspBackendChoice makeChoice(
    const ApplicationState &state, pipetune::DspBackendKind kind) {
  auto label = std::string(backendDisplayName(kind));
  if (!state.hasRuntimeStatus) {
    label += " — availability unknown";
    return {.kind = kind,
            .label = std::move(label),
            .availabilityKnown = false,
            .available = false};
  }

  const auto *availability =
      backendAvailability(state.runtime, kind);
  if (availability == nullptr) {
    label += " — availability not reported";
    return {.kind = kind,
            .label = std::move(label),
            .availabilityKnown = false,
            .available = false};
  }
  label += availability->available ? " — available" : " — unavailable";
  label += "; CPU: ";
  label += availability->cpuRequirement.empty()
               ? std::string("unknown")
               : availability->cpuRequirement;
  if (!availability->error.empty()) {
    label += " — ";
    label += availability->error;
  }
  return {.kind = kind,
          .label = std::move(label),
          .availabilityKnown = true,
          .available = availability->available};
}

static std::string effectiveBackendText(
    const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return "Effective backend unavailable";
  }

  auto text = "Configured " +
              std::string(backendDisplayName(
                  state.runtime.configuredDspBackend)) +
              "  •  Effective ";
  if (state.runtime.effectiveDspBackend.has_value()) {
    text += backendDisplayName(
        *state.runtime.effectiveDspBackend);
  } else {
    text += "unavailable";
  }
  if (state.runtime.dspBackendFallback) {
    text += "  •  Fallback";
  }
  if (!state.runtime.dspBackendError.empty()) {
    text += " — ";
    text += state.runtime.dspBackendError;
  }
  return text;
}

DspBackendSelectionPresentation
makeDspBackendSelectionPresentation(
    const ApplicationState &state,
    pipetune::DspBackendKind editedBackend) {
  constexpr auto kinds = std::array{
      pipetune::DspBackendKind::scalar,
      pipetune::DspBackendKind::simd,
  };
  auto choices = std::vector<DspBackendChoice>{};
  choices.reserve(kinds.size());
  auto activeIndex = std::size_t{0};
  auto selectedAvailable = false;
  for (const auto kind : kinds) {
    auto choice = makeChoice(state, kind);
    if (kind == editedBackend) {
      activeIndex = choices.size();
      selectedAvailable = choice.availabilityKnown &&
                          choice.available;
    }
    choices.push_back(std::move(choice));
  }

  return {
      .choices = std::move(choices),
      .activeIndex = activeIndex,
      .effectiveBackend = effectiveBackendText(state),
      .selectedBackendAvailable = selectedAvailable,
      .sensitive =
          state.connection == ControlConnectionState::connected &&
          state.hasRuntimeStatus && !state.operationPending &&
          !state.runtime.rateTransitioning,
  };
}

} // namespace pipetune_gtk
