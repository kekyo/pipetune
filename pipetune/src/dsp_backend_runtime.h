#ifndef PIPETUNE_DSP_BACKEND_RUNTIME_H
#define PIPETUNE_DSP_BACKEND_RUNTIME_H

#include "dsp_pipeline_slot.h"

#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_pipeline.h"

#include <optional>
#include <string>
#include <vector>

namespace pipetune {

/**
 * Holds backend discovery and selection state retained by the daemon.
 */
struct DspBackendRuntimeState {
  /** Backends discovered once at daemon startup. */
  DspBackends backends;
  /** Startup or successfully applied user choice. */
  DspBackendKind configuredBackend;
  /** Startup or successfully applied SIMD dispatch preference. */
  DspSimdVariant configuredSimdVariant;
  /** Effective backend, or no value without a usable scalar backend. */
  std::optional<DspBackendKind> effectiveBackend;
  /** Effective concrete variant, or no value without a usable backend. */
  std::optional<DspBackendVariant> effectiveVariant;
  /** True when a lower tier or scalar replaced the preferred SIMD tier. */
  bool fallback;
  /** Current selection diagnostic. */
  std::string error;
};

/**
 * Reports a transactional live backend switch.
 */
struct DspBackendSwitchResult {
  /** True when the runtime selection or active pipeline changed. */
  bool changed;
  /** Non-fatal preset nodes omitted while rebuilding. */
  std::vector<PipelineWarning> warnings;
  /** Rejection or rebuild diagnostic. */
  std::string error;
};

/**
 * Creates runtime state using the startup fallback rules.
 *
 * @param backends Scalar and SIMD discovery results.
 * @param configuredBackend Persisted backend choice.
 * @return Effective selection and retained discovery results.
 */
DspBackendRuntimeState
makeDspBackendRuntimeState(DspBackends backends,
                           DspBackendKind configuredBackend);

/**
 * Creates runtime state using logical kind and SIMD pinning rules.
 *
 * @param backends Scalar and SIMD discovery results.
 * @param configuredBackend Persisted backend choice.
 * @param configuredSimdVariant Persisted SIMD dispatch preference.
 * @return Effective selection and retained discovery results.
 */
DspBackendRuntimeState
makeDspBackendRuntimeState(DspBackends backends,
                           DspBackendKind configuredBackend,
                           DspSimdVariant configuredSimdVariant);

/**
 * Transactionally rebuilds a preset pipeline with another backend.
 *
 * Bypass mode updates only the retained selection. A failed availability
 * check or rebuild leaves both the pipeline and runtime state unchanged.
 *
 * @param pipeline Active pipeline slot.
 * @param state Mutable runtime backend state.
 * @param requestedBackend Requested live backend.
 * @param options Current DSP processing format.
 * @param rateTransitioning True while a rate rebuild transaction is active.
 * @return Change marker, rebuild warnings, and diagnostic.
 */
DspBackendSwitchResult
switchDspBackend(DspPipelineSlot &pipeline,
                 DspBackendRuntimeState &state,
                 DspBackendKind requestedBackend,
                 const PipelineBuildOptions &options,
                 bool rateTransitioning);

/**
 * Transactionally applies a logical backend and SIMD dispatch preference.
 *
 * @param pipeline Active pipeline slot.
 * @param state Mutable runtime backend state.
 * @param requestedBackend Requested logical backend.
 * @param requestedSimdVariant Requested automatic or pinned SIMD preference.
 * @param options Current DSP processing format.
 * @param rateTransitioning True while a rate rebuild transaction is active.
 * @return Change marker, rebuild warnings, and diagnostic.
 */
DspBackendSwitchResult
switchDspBackend(DspPipelineSlot &pipeline,
                 DspBackendRuntimeState &state,
                 DspBackendKind requestedBackend,
                 DspSimdVariant requestedSimdVariant,
                 const PipelineBuildOptions &options,
                 bool rateTransitioning);

} // namespace pipetune

#endif
