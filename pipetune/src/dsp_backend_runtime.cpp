/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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

DspBackendSwitchPlan planDspBackendSwitch(DspBackendRuntimeState state,
    DspBackendKind requestedBackend, DspSimdVariant requestedSimdVariant, bool rateTransitioning) {
  if (requestedBackend != DspBackendKind::scalar &&
      requestedBackend != DspBackendKind::simd) {
    return {.state = std::move(state), .backend = {}, .error = "requested DSP backend is invalid"};
  }
  if (dspSimdVariantName(requestedSimdVariant).empty() ||
      (requestedBackend == DspBackendKind::scalar &&
       requestedSimdVariant != DspSimdVariant::automatic)) {
    return {.state = std::move(state), .backend = {}, .error = "requested DSP SIMD variant is invalid"};
  }
  if (rateTransitioning) {
    return {.state = std::move(state), .backend = {}, .error =
            "cannot change DSP backend during sample-rate transition",
    };
  }
  const auto selected = selectDspBackend(
      requestedBackend, requestedSimdVariant, state.backends);
  if (selected.effectiveBackend == nullptr) {
    return {.state = state, .backend = {}, .error = selected.error.empty()
                     ? unavailableError(DspBackendKind::scalar,
                                        state.backends.scalar)
                     : selected.error,
    };
  }
  if (selected.effectiveBackend->kind() != requestedBackend) {
    return {.state = std::move(state), .backend = {}, .error = selected.error.empty()
                     ? std::string(dspBackendName(requestedBackend)) +
                           " DSP backend is unavailable"
                     : selected.error,
    };
  }

  const auto changed = state.configuredBackend != requestedBackend ||
      state.configuredSimdVariant != requestedSimdVariant ||
      state.effectiveBackend != requestedBackend || state.effectiveVariant != selected.effectiveVariant ||
      state.fallback != selected.fallback || state.error != selected.error;
  const auto rebuild = state.effectiveVariant != selected.effectiveVariant;
  state.configuredBackend = requestedBackend;
  state.configuredSimdVariant = requestedSimdVariant;
  state.effectiveBackend = requestedBackend;
  state.effectiveVariant = selected.effectiveVariant;
  state.fallback = selected.fallback;
  state.error = selected.error;
  return {std::move(state), selected.effectiveBackend, changed, rebuild, {}};
}

DspBackendSwitchResult switchDspBackend(DspPipelineSlot &pipeline, DspBackendRuntimeState &state,
    DspBackendKind requestedBackend, DspSimdVariant requestedSimdVariant,
    const PipelineBuildOptions &options, bool rateTransitioning) {
  auto plan = planDspBackendSwitch(state, requestedBackend, requestedSimdVariant, rateTransitioning);
  if (!plan.error.empty()) return {false, {}, std::move(plan.error)};
  auto warnings = std::vector<PipelineWarning>{};
  if (plan.rebuild && pipeline.backendKind().has_value()) {
    auto rebuilt = pipeline.rebuildActive(options, plan.backend);
    if (!rebuilt.pipeline) return {false, {}, std::move(rebuilt.error)};
    warnings = std::move(rebuilt.warnings);
    pipeline.replace(std::move(rebuilt.pipeline));
  }
  state = std::move(plan.state);
  return {plan.changed, std::move(warnings), {}};
}

} // namespace pipetune
