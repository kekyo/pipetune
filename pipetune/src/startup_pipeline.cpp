#include "startup_pipeline.h"

#include "pipetune/startup_config.h"

#include <string>
#include <utility>

namespace pipetune {

static StartupPipelineResult
prepareBypass(const PipelineBuildOptions &options,
              std::string preferredOutput,
              SampleRatePolicy ratePolicy,
              std::string configurationError) {
  auto created = createBypassDspPipeline(options);
  if (created.pipeline == nullptr) {
    return {.pipeline = nullptr,
            .activePresetPath = {},
            .preferredOutput = std::move(preferredOutput),
            .ratePolicy = ratePolicy,
            .configurationError = std::move(configurationError),
            .warnings = {},
            .error = std::move(created.error)};
  }
  return {.pipeline = std::move(created.pipeline),
          .activePresetPath = {},
          .preferredOutput = std::move(preferredOutput),
          .ratePolicy = ratePolicy,
          .configurationError = std::move(configurationError),
          .warnings = {},
          .error = {}};
}

StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options) {
  const auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return prepareBypass(
        options, {}, defaultSampleRatePolicy(), configured.error);
  }
  if (!configured.presetFound) {
    return prepareBypass(options, configured.preferredOutput,
                         configured.ratePolicy, {});
  }

  auto loaded = loadDspPipeline(configured.presetPath, options);
  if (loaded.pipeline == nullptr) {
    return prepareBypass(
        options, configured.preferredOutput, configured.ratePolicy,
        "cannot load configured preset: " + loaded.error);
  }
  return {.pipeline = std::move(loaded.pipeline),
          .activePresetPath = configured.presetPath,
          .preferredOutput = configured.preferredOutput,
          .ratePolicy = configured.ratePolicy,
          .configurationError = {},
          .warnings = std::move(loaded.warnings),
          .error = {}};
}

} // namespace pipetune
