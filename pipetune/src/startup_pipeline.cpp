#include "startup_pipeline.h"

#include "pipetune/startup_config.h"

#include <string>
#include <utility>

namespace pipetune {

static StartupPipelineResult
prepareBypass(const PipelineBuildOptions &options,
              SampleRatePolicy ratePolicy, std::string configurationError,
              DspBackends backends,
              const DspBackendSelection &selection) {
  auto created = createBypassDspPipeline(options);
  if (created.pipeline == nullptr) {
    return {.pipeline = nullptr,
            .activePresetPath = {},
            .ratePolicy = ratePolicy,
            .configurationError = std::move(configurationError),
            .warnings = {},
            .error = std::move(created.error),
            .dspBackends = std::move(backends),
            .configuredDspBackend = selection.configuredBackend,
            .configuredDspSimdVariant =
                selection.configuredSimdVariant,
            .effectiveDspBackend =
                selection.effectiveBackend == nullptr
                    ? std::optional<DspBackendKind>{}
                    : selection.effectiveBackend->kind(),
            .effectiveDspVariant = selection.effectiveVariant,
            .dspBackendFallback = selection.fallback,
            .dspBackendError = selection.error};
  }
  return {.pipeline = std::move(created.pipeline),
          .activePresetPath = {},
          .ratePolicy = ratePolicy,
          .configurationError = std::move(configurationError),
          .warnings = {},
          .error = {},
          .dspBackends = std::move(backends),
          .configuredDspBackend = selection.configuredBackend,
          .configuredDspSimdVariant =
              selection.configuredSimdVariant,
          .effectiveDspBackend =
              selection.effectiveBackend == nullptr
                  ? std::optional<DspBackendKind>{}
                  : selection.effectiveBackend->kind(),
          .effectiveDspVariant = selection.effectiveVariant,
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
        selectDspBackend(DspBackendKind::scalar,
                         DspSimdVariant::automatic, backends);
    return prepareBypass(options, defaultSampleRatePolicy(), configured.error,
                         std::move(backends), selection);
  }
  const auto &config = configured.config;
  const auto selection =
      selectDspBackend(config.dspBackend, config.dspSimdVariant, backends);
  if (!config.presetFound) {
    return prepareBypass(options, config.ratePolicy, {}, std::move(backends),
                         selection);
  }
  if (selection.effectiveBackend == nullptr) {
    return prepareBypass(
        options, config.ratePolicy,
        "cannot load configured preset: " + selection.error,
        std::move(backends), selection);
  }

  auto loaded = loadDspPipeline(config.presetPath, options,
                                selection.effectiveBackend);
  if (loaded.pipeline == nullptr) {
    return prepareBypass(
        options, config.ratePolicy,
        "cannot load configured preset: " + loaded.error,
        std::move(backends), selection);
  }
  return {.pipeline = std::move(loaded.pipeline),
          .activePresetPath = config.presetPath,
          .ratePolicy = config.ratePolicy,
          .configurationError = {},
          .warnings = std::move(loaded.warnings),
          .error = {},
          .dspBackends = std::move(backends),
          .configuredDspBackend = selection.configuredBackend,
          .configuredDspSimdVariant =
              selection.configuredSimdVariant,
          .effectiveDspBackend = selection.effectiveBackend->kind(),
          .effectiveDspVariant = selection.effectiveVariant,
          .dspBackendFallback = selection.fallback,
          .dspBackendError = selection.error};
}

} // namespace pipetune
