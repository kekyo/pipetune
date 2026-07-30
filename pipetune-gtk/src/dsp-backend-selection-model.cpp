#include "dsp-backend-selection-model.h"

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune_gtk {

static std::string_view backendDisplayName(
    pipetune::DspBackendKind kind) {
  return kind == pipetune::DspBackendKind::scalar
             ? std::string_view("Scalar")
             : std::string_view("SIMD");
}

static std::string_view simdVariantDisplayName(
    pipetune::DspSimdVariant variant) {
  switch (variant) {
  case pipetune::DspSimdVariant::automatic:
    return "Automatic";
  case pipetune::DspSimdVariant::baseline:
    return "Baseline";
  case pipetune::DspSimdVariant::x86_64_v3:
    return "x86-64-v3";
  case pipetune::DspSimdVariant::x86_64_v4:
    return "x86-64-v4";
  case pipetune::DspSimdVariant::arm64Sve:
    return "sve";
  }
  return "Unknown";
}

static std::optional<pipetune::DspSimdVariant>
simdPreferenceForVariant(pipetune::DspBackendVariant variant) {
  switch (variant) {
  case pipetune::DspBackendVariant::scalar:
    return std::nullopt;
  case pipetune::DspBackendVariant::simdBaseline:
    return pipetune::DspSimdVariant::baseline;
  case pipetune::DspBackendVariant::x86_64_v3:
    return pipetune::DspSimdVariant::x86_64_v3;
  case pipetune::DspBackendVariant::x86_64_v4:
    return pipetune::DspSimdVariant::x86_64_v4;
  case pipetune::DspBackendVariant::arm64Sve:
    return pipetune::DspSimdVariant::arm64Sve;
  }
  return std::nullopt;
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

static const pipetune::ControlDspVariantAvailability *
variantAvailability(const pipetune::ControlRuntimeStatus &status,
                    pipetune::DspSimdVariant variant) {
  const auto concrete = pipetune::concreteDspBackendVariant(variant);
  if (!concrete.has_value()) {
    return nullptr;
  }
  for (const auto &availability : status.availableDspVariants) {
    if (availability.variant == *concrete) {
      return &availability;
    }
  }
  return nullptr;
}

static void appendAvailability(std::string &label, bool available,
                               std::string_view cpuRequirement,
                               std::string_view error) {
  label += available ? " — available" : " — unavailable";
  label += "; CPU: ";
  label += cpuRequirement.empty() ? "unknown" : cpuRequirement;
  if (!error.empty()) {
    label += " — ";
    label += error;
  }
}

static DspBackendChoice makeChoice(
    const ApplicationState &state, pipetune::DspBackendKind kind,
    pipetune::DspSimdVariant simdVariant) {
  auto label = std::string(backendDisplayName(kind));
  if (kind == pipetune::DspBackendKind::simd) {
    label += " ";
    label += simdVariantDisplayName(simdVariant);
  }
  if (!state.hasRuntimeStatus) {
    label += " — availability unknown";
    return {.kind = kind,
            .simdVariant = simdVariant,
            .label = std::move(label),
            .availabilityKnown = false,
            .available = false};
  }

  if (kind == pipetune::DspBackendKind::scalar ||
      simdVariant == pipetune::DspSimdVariant::automatic) {
    const auto *availability =
        backendAvailability(state.runtime, kind);
    if (availability == nullptr) {
      label += " — availability not reported";
      return {.kind = kind,
              .simdVariant = simdVariant,
              .label = std::move(label),
              .availabilityKnown = false,
              .available = false};
    }
    appendAvailability(label, availability->available,
                       availability->cpuRequirement,
                       availability->error);
    return {.kind = kind,
            .simdVariant = simdVariant,
            .label = std::move(label),
            .availabilityKnown = true,
            .available = availability->available};
  }

  const auto *availability =
      variantAvailability(state.runtime, simdVariant);
  if (availability == nullptr) {
    label += " — availability not reported";
    return {.kind = kind,
            .simdVariant = simdVariant,
            .label = std::move(label),
            .availabilityKnown = false,
            .available = false};
  }
  appendAvailability(label, availability->available,
                     availability->cpuRequirement,
                     availability->error);
  return {.kind = kind,
          .simdVariant = simdVariant,
          .label = std::move(label),
          .availabilityKnown = true,
          .available = availability->available};
}

static std::vector<pipetune::DspSimdVariant>
applicableSimdVariants(const ApplicationState &state,
                       pipetune::DspSimdVariant editedVariant) {
  auto variants = std::vector<pipetune::DspSimdVariant>{
      pipetune::DspSimdVariant::automatic,
      pipetune::DspSimdVariant::baseline};
  if (state.hasRuntimeStatus) {
    for (const auto &availability :
         state.runtime.availableDspVariants) {
      const auto preference =
          simdPreferenceForVariant(availability.variant);
      if (preference.has_value() &&
          std::find(variants.begin(), variants.end(), *preference) ==
              variants.end()) {
        variants.push_back(*preference);
      }
    }
  } else {
#if defined(__x86_64__)
    variants.push_back(pipetune::DspSimdVariant::x86_64_v3);
    variants.push_back(pipetune::DspSimdVariant::x86_64_v4);
#elif defined(__i386__)
    variants.push_back(pipetune::DspSimdVariant::x86_64_v3);
#elif defined(__aarch64__)
    variants.push_back(pipetune::DspSimdVariant::arm64Sve);
#endif
  }
  if (editedVariant != pipetune::DspSimdVariant::automatic &&
      std::find(variants.begin(), variants.end(), editedVariant) ==
          variants.end()) {
    variants.push_back(editedVariant);
  }
  return variants;
}

static std::string effectiveBackendText(
    const ApplicationState &state) {
  if (!state.hasRuntimeStatus) {
    return "Effective backend unavailable";
  }

  auto text = std::string("Configured ");
  text += backendDisplayName(state.runtime.configuredDspBackend);
  if (state.runtime.configuredDspBackend ==
      pipetune::DspBackendKind::simd) {
    text += " (";
    text += simdVariantDisplayName(
        state.runtime.configuredDspSimdVariant);
    text += ")";
  }
  text += "  •  Effective ";
  if (state.runtime.effectiveDspVariant.has_value()) {
    text += pipetune::dspBackendVariantName(
        *state.runtime.effectiveDspVariant);
  } else if (state.runtime.effectiveDspBackend.has_value()) {
    text += pipetune::dspBackendName(
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
    pipetune::DspBackendKind editedBackend,
    pipetune::DspSimdVariant editedSimdVariant) {
  auto choices = std::vector<DspBackendChoice>{};
  const auto variants =
      applicableSimdVariants(state, editedSimdVariant);
  choices.reserve(variants.size() + 1);
  auto activeIndex = std::size_t{0};
  auto selectedAvailable = false;
  choices.push_back(makeChoice(
      state, pipetune::DspBackendKind::scalar,
      pipetune::DspSimdVariant::automatic));
  if (editedBackend == pipetune::DspBackendKind::scalar) {
    selectedAvailable = choices.front().availabilityKnown &&
                        choices.front().available;
  }
  for (const auto variant : variants) {
    auto choice = makeChoice(state, pipetune::DspBackendKind::simd,
                             variant);
    if (editedBackend == pipetune::DspBackendKind::simd &&
        variant == editedSimdVariant) {
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
