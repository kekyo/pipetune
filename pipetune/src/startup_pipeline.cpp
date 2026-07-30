#include "startup_pipeline.h"

#include "pipetune/startup_config.h"

#include <string>
#include <utility>

namespace pipetune {

static StartupPipelineResult
prepareBypass(const PipelineBuildOptions &options,
              std::string preferredOutput, SampleRatePolicy ratePolicy,
              std::string configurationError, DspBackends backends,
              const DspBackendSelection &selection) {
  auto created = createBypassDspPipeline(options);
  if (created.pipeline == nullptr) {
    return {.pipeline = nullptr,
            .activePresetPath = {},
            .preferredOutput = std::move(preferredOutput),
            .ratePolicy = ratePolicy,
            .configurationError = std::move(configurationError),
            .warnings = {},
            .error = std::move(created.error),
            .dspBackends = std::move(backends),
            .configuredDspBackend = selection.configuredBackend,
            .effectiveDspBackend =
                selection.effectiveBackend == nullptr
                    ? std::optional<DspBackendKind>{}
                    : selection.effectiveBackend->kind(),
            .dspBackendFallback = selection.fallback,
            .dspBackendError = selection.error};
  }
  return {.pipeline = std::move(created.pipeline),
          .activePresetPath = {},
          .preferredOutput = std::move(preferredOutput),
          .ratePolicy = ratePolicy,
          .configurationError = std::move(configurationError),
          .warnings = {},
          .error = {},
          .dspBackends = std::move(backends),
          .configuredDspBackend = selection.configuredBackend,
          .effectiveDspBackend =
              selection.effectiveBackend == nullptr
                  ? std::optional<DspBackendKind>{}
                  : selection.effectiveBackend->kind(),
          .dspBackendFallback = selection.fallback,
          .dspBackendError = selection.error};
}

StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options) {
  return prepareStartupPipeline(configPath, options, discoverDspBackends());
}

StartupPipelineResult
prepareStartupPipeline(const std::filesystem::path &configPath,
                       const PipelineBuildOptions &options,
                       DspBackends backends) {
  const auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    const auto selection =
        selectDspBackend(DspBackendKind::scalar, backends);
    return prepareBypass(options, {}, defaultSampleRatePolicy(),
                         configured.error, std::move(backends), selection);
  }
  const auto selection =
      selectDspBackend(configured.dspBackend, backends);
  if (!configured.presetFound) {
    return prepareBypass(options, configured.preferredOutput,
                         configured.ratePolicy, {}, std::move(backends),
                         selection);
  }
  if (selection.effectiveBackend == nullptr) {
    return prepareBypass(
        options, configured.preferredOutput, configured.ratePolicy,
        "cannot load configured preset: " + selection.error,
        std::move(backends), selection);
  }

  auto loaded = loadDspPipeline(configured.presetPath, options,
                                selection.effectiveBackend);
  if (loaded.pipeline == nullptr) {
    return prepareBypass(
        options, configured.preferredOutput, configured.ratePolicy,
        "cannot load configured preset: " + loaded.error,
        std::move(backends), selection);
  }
  return {.pipeline = std::move(loaded.pipeline),
          .activePresetPath = configured.presetPath,
          .preferredOutput = configured.preferredOutput,
          .ratePolicy = configured.ratePolicy,
          .configurationError = {},
          .warnings = std::move(loaded.warnings),
          .error = {},
          .dspBackends = std::move(backends),
          .configuredDspBackend = selection.configuredBackend,
          .effectiveDspBackend = selection.effectiveBackend->kind(),
          .dspBackendFallback = selection.fallback,
          .dspBackendError = selection.error};
}

} // namespace pipetune
