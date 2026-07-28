#include "startup_pipeline.h"

#include "pipetune/startup_config.h"

#include <string>
#include <utility>

namespace pipetune {

static StartupPipelineResult
prepareBypass(const PipelineBuildOptions &options,
              std::string preferredOutput,
              std::string configurationError) {
  auto created = createBypassDspPipeline(options);
  if (created.pipeline == nullptr) {
    return {.pipeline = nullptr,
            .activePresetPath = {},
            .preferredOutput = std::move(preferredOutput),
            .configurationError = std::move(configurationError),
            .warnings = {},
            .error = std::move(created.error)};
  }
  return {.pipeline = std::move(created.pipeline),
          .activePresetPath = {},
          .preferredOutput = std::move(preferredOutput),
          .configurationError = std::move(configurationError),
          .warnings = {},
          .error = {}};
}

StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options) {
  const auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return prepareBypass(options, {}, configured.error);
  }
  if (!configured.presetFound) {
    return prepareBypass(options, configured.preferredOutput, {});
  }

  auto loaded = loadDspPipeline(configured.presetPath, options);
  if (loaded.pipeline == nullptr) {
    return prepareBypass(
        options, configured.preferredOutput,
        "cannot load configured preset: " + loaded.error);
  }
  return {.pipeline = std::move(loaded.pipeline),
          .activePresetPath = configured.presetPath,
          .preferredOutput = configured.preferredOutput,
          .configurationError = {},
          .warnings = std::move(loaded.warnings),
          .error = {}};
}

} // namespace pipetune
