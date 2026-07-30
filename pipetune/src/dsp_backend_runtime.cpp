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
  const auto selected = selectDspBackend(configuredBackend, backends);
  return {
      .backends = std::move(backends),
      .configuredBackend = configuredBackend,
      .effectiveBackend =
          selected.effectiveBackend == nullptr
              ? std::optional<DspBackendKind>{}
              : selected.effectiveBackend->kind(),
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
  if (requestedBackend != DspBackendKind::scalar &&
      requestedBackend != DspBackendKind::simd) {
    return {.changed = false,
            .warnings = {},
            .error = "requested DSP backend is invalid"};
  }
  if (rateTransitioning) {
    return {
        .changed = false,
        .warnings = {},
        .error =
            "cannot change DSP backend during sample-rate transition",
    };
  }
  if (state.backends.scalar.backend == nullptr) {
    return {
        .changed = false,
        .warnings = {},
        .error = unavailableError(DspBackendKind::scalar,
                                  state.backends.scalar),
    };
  }
  const auto &requested = state.backends.get(requestedBackend);
  if (requested.backend == nullptr) {
    return {
        .changed = false,
        .warnings = {},
        .error = unavailableError(requestedBackend, requested),
    };
  }

  const auto stateChanged =
      state.configuredBackend != requestedBackend ||
      state.effectiveBackend != requestedBackend || state.fallback ||
      !state.error.empty();
  if (state.effectiveBackend == requestedBackend) {
    state.configuredBackend = requestedBackend;
    state.effectiveBackend = requestedBackend;
    state.fallback = false;
    state.error.clear();
    return {.changed = stateChanged, .warnings = {}, .error = {}};
  }

  auto warnings = std::vector<PipelineWarning>{};
  if (pipeline.backendKind().has_value()) {
    auto rebuilt =
        pipeline.rebuildActive(options, requested.backend);
    if (rebuilt.pipeline == nullptr) {
      return {.changed = false,
              .warnings = {},
              .error = std::move(rebuilt.error)};
    }
    warnings = std::move(rebuilt.warnings);
    pipeline.replace(std::move(rebuilt.pipeline));
  }

  state.configuredBackend = requestedBackend;
  state.effectiveBackend = requestedBackend;
  state.fallback = false;
  state.error.clear();
  return {.changed = true,
          .warnings = std::move(warnings),
          .error = {}};
}

} // namespace pipetune
