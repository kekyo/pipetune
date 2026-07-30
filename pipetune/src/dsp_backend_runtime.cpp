#include "dsp_backend_runtime.h"

#include <utility>

namespace pipetune {

static std::string unavailableError(
    DspBackendKind kind, const DspBackendLoadResult &loaded) {
  if (!loaded.error.empty()) {
    return loaded.error;
  }
  return std::string(dspBackendName(kind)) +
         " DSP backend is unavailable";
}

DspBackendRuntimeState
makeDspBackendRuntimeState(DspBackends backends,
                           DspBackendKind configuredBackend) {
  return makeDspBackendRuntimeState(
      std::move(backends), configuredBackend,
      DspSimdVariant::automatic);
}

DspBackendRuntimeState
makeDspBackendRuntimeState(DspBackends backends,
                           DspBackendKind configuredBackend,
                           DspSimdVariant configuredSimdVariant) {
  const auto selected = selectDspBackend(
      configuredBackend, configuredSimdVariant, backends);
  return {
      .backends = std::move(backends),
      .configuredBackend = configuredBackend,
      .configuredSimdVariant = configuredSimdVariant,
      .effectiveBackend =
          selected.effectiveBackend == nullptr
              ? std::optional<DspBackendKind>{}
              : selected.effectiveBackend->kind(),
      .effectiveVariant = selected.effectiveVariant,
      .fallback = selected.fallback,
      .error = selected.error,
  };
}

DspBackendSwitchResult
switchDspBackend(DspPipelineSlot &pipeline,
                 DspBackendRuntimeState &state,
                 DspBackendKind requestedBackend,
                 const PipelineBuildOptions &options,
                 bool rateTransitioning) {
  return switchDspBackend(
      pipeline, state, requestedBackend, DspSimdVariant::automatic,
      options, rateTransitioning);
}

DspBackendSwitchResult
switchDspBackend(DspPipelineSlot &pipeline,
                 DspBackendRuntimeState &state,
                 DspBackendKind requestedBackend,
                 DspSimdVariant requestedSimdVariant,
                 const PipelineBuildOptions &options,
                 bool rateTransitioning) {
  if (requestedBackend != DspBackendKind::scalar &&
      requestedBackend != DspBackendKind::simd) {
    return {.changed = false,
            .warnings = {},
            .error = "requested DSP backend is invalid"};
  }
  if (dspSimdVariantName(requestedSimdVariant).empty() ||
      (requestedBackend == DspBackendKind::scalar &&
       requestedSimdVariant != DspSimdVariant::automatic)) {
    return {.changed = false,
            .warnings = {},
            .error = "requested DSP SIMD variant is invalid"};
  }
  if (rateTransitioning) {
    return {
        .changed = false,
        .warnings = {},
        .error =
            "cannot change DSP backend during sample-rate transition",
    };
  }
  const auto selected = selectDspBackend(
      requestedBackend, requestedSimdVariant, state.backends);
  if (selected.effectiveBackend == nullptr) {
    return {
        .changed = false,
        .warnings = {},
        .error = selected.error.empty()
                     ? unavailableError(DspBackendKind::scalar,
                                        state.backends.scalar)
                     : selected.error,
    };
  }
  if (selected.effectiveBackend->kind() != requestedBackend) {
    return {
        .changed = false,
        .warnings = {},
        .error = selected.error.empty()
                     ? std::string(dspBackendName(requestedBackend)) +
                           " DSP backend is unavailable"
                     : selected.error,
    };
  }

  const auto stateChanged =
      state.configuredBackend != requestedBackend ||
      state.configuredSimdVariant != requestedSimdVariant ||
      state.effectiveBackend != requestedBackend ||
      state.effectiveVariant != selected.effectiveVariant ||
      state.fallback != selected.fallback ||
      state.error != selected.error;
  if (state.effectiveVariant == selected.effectiveVariant) {
    state.configuredBackend = requestedBackend;
    state.configuredSimdVariant = requestedSimdVariant;
    state.effectiveBackend = requestedBackend;
    state.effectiveVariant = selected.effectiveVariant;
    state.fallback = selected.fallback;
    state.error = selected.error;
    return {.changed = stateChanged, .warnings = {}, .error = {}};
  }

  auto warnings = std::vector<PipelineWarning>{};
  if (pipeline.backendKind().has_value()) {
    auto rebuilt =
        pipeline.rebuildActive(options, selected.effectiveBackend);
    if (rebuilt.pipeline == nullptr) {
      return {.changed = false,
              .warnings = {},
              .error = std::move(rebuilt.error)};
    }
    warnings = std::move(rebuilt.warnings);
    pipeline.replace(std::move(rebuilt.pipeline));
  }

  state.configuredBackend = requestedBackend;
  state.configuredSimdVariant = requestedSimdVariant;
  state.effectiveBackend = requestedBackend;
  state.effectiveVariant = selected.effectiveVariant;
  state.fallback = selected.fallback;
  state.error = selected.error;
  return {.changed = true,
          .warnings = std::move(warnings),
          .error = {}};
}

} // namespace pipetune
