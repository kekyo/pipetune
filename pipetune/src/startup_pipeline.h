#ifndef PIPETUNE_STARTUP_PIPELINE_H
#define PIPETUNE_STARTUP_PIPELINE_H

#include "pipetune/dsp_pipeline.h"
#include "pipetune/sample_rate.h"

#include <filesystem>
#include <memory>
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
  /** Persisted preferred PipeWire node.name, or empty for system default. */
  std::string preferredOutput;
  /** Persisted Max/fixed and suggest/force sample-rate choice. */
  SampleRatePolicy ratePolicy = {};
  /** Recoverable configuration diagnostic reported while bypassing. */
  std::string configurationError;
  /** Non-fatal preset node diagnostics. */
  std::vector<PipelineWarning> warnings;
  /** Fatal pipeline construction diagnostic. */
  std::string error;
};

/**
 * Prepares a preset pipeline or a fail-open bypass from startup configuration.
 *
 * Missing configuration selects bypass without a diagnostic. Invalid
 * configuration or an unloadable configured preset selects bypass and records
 * a configuration diagnostic.
 *
 * @param configPath Startup configuration file path.
 * @param options Maximum processing format for the prepared pipeline.
 * @return Prepared startup pipeline and diagnostics.
 */
StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options);

} // namespace pipetune

#endif
