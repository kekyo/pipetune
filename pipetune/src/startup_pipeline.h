/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_STARTUP_PIPELINE_H
#define PIPETUNE_STARTUP_PIPELINE_H

#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_idle.h"
#include "pipetune/dsp_pipeline.h"
#include "pipetune/sample_rate.h"

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace pipetune {

/**
 * Holds the pipeline selected from one optional startup configuration.
 */
struct StartupPipelineResult {
  /** Prepared preset or bypass pipeline, or null after a fatal error. */
  std::unique_ptr<DspPipeline> pipeline;
  /** Active absolute preset path, or empty in bypass mode. */
  std::filesystem::path activePresetPath;
  /** Persisted automatic/fixed graph-rate choice. */
  SampleRatePolicy ratePolicy = {};
  /** Persisted automatic DSP suspension policy. */
  DspIdlePolicy dspIdlePolicy = {};
  /** Recoverable configuration diagnostic reported while bypassing. */
  std::string configurationError;
  /** Non-fatal preset node diagnostics. */
  std::vector<PipelineWarning> warnings;
  /** Fatal pipeline construction diagnostic. */
  std::string error;
  /** Independently discovered scalar and SIMD backend variants. */
  DspBackends dspBackends = {};
  /** Persisted backend choice. */
  DspBackendKind configuredDspBackend = DspBackendKind::scalar;
  /** Persisted SIMD dispatch preference. */
  DspSimdVariant configuredDspSimdVariant =
      DspSimdVariant::automatic;
  /** Active backend, or no value when the mandatory scalar backend failed. */
  std::optional<DspBackendKind> effectiveDspBackend =
      DspBackendKind::scalar;
  /** Active concrete variant, or no value when no backend is usable. */
  std::optional<DspBackendVariant> effectiveDspVariant =
      DspBackendVariant::scalar;
  /** True when a lower tier or scalar replaced the preferred SIMD tier. */
  bool dspBackendFallback = false;
  /** Backend availability or compatibility diagnostic. */
  std::string dspBackendError = {};
};

/**
 * Prepares a preset pipeline or a fail-open bypass from startup configuration.
 *
 * Missing configuration selects bypass without a diagnostic. Invalid
 * configuration or an unloadable configured preset selects bypass and records
 * a configuration diagnostic.
 *
 * @param configPath Startup configuration file path.
 * @param options Automatic-mode initial rate and maximum processing format.
 * A fixed configured policy replaces options.sampleRate before construction.
 * @return Prepared startup pipeline and diagnostics.
 */
StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options);

/**
 * Prepares startup state from an explicitly supplied backend discovery result.
 *
 * This overload is also useful to present a stable discovery snapshot to
 * callers that will retain it for live backend switching.
 *
 * @param configPath Startup configuration file path.
 * @param options Automatic-mode initial rate and maximum processing format.
 * A fixed configured policy replaces options.sampleRate before construction.
 * @param backends Independently discovered scalar and SIMD backends.
 * @return Prepared startup pipeline and diagnostics.
 */
StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options,
                       DspBackends backends);

} // namespace pipetune

#endif
