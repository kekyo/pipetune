/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "startup_pipeline.h"
#include "preparation_dispatcher.h"

#include "pipetune/startup_config.h"

#include <string>
#include <utility>

namespace pipetune {

static PipelineBuildOptions startupBuildOptions(
    const PipelineBuildOptions &options,
    const StartupConfig &config) {
  auto resolved = options;
  resolved.maxChannels = outputDspChannelCount(config.outputConfiguration);
  if (config.ratePolicy.mode == SampleRateMode::fixed) {
    resolved.sampleRate = static_cast<float>(config.ratePolicy.fixedRate);
  }
  return resolved;
}

static StartupPipelineResult
prepareBypass(const PipelineBuildOptions &options,
              const StartupConfig &config,
              std::string configurationError, DspBackends backends,
              const DspBackendSelection &selection) {
  auto created = createBypassDspPipeline(options);
  if (created.pipeline == nullptr) {
    return {.pipeline = nullptr,
            .activePresetPath = {},
            .ratePolicy = config.ratePolicy,
            .dspIdlePolicy = config.dspIdlePolicy,
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
            .dspBackendError = selection.error,
            .outputConfiguration = config.outputConfiguration};
  }
  return {.pipeline = std::move(created.pipeline),
          .activePresetPath = {},
          .ratePolicy = config.ratePolicy,
          .dspIdlePolicy = config.dspIdlePolicy,
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
          .dspBackendError = selection.error,
          .outputConfiguration = config.outputConfiguration};
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
  return runPreparation<StartupPipelineResult>([&] {
    return prepareStartupPipelineAsync(configPath, options, std::move(backends), {});
  });
}

cardio::promise<StartupPipelineResult> prepareStartupPipelineAsync(
    std::filesystem::path configPath, PipelineBuildOptions options,
    DspBackends backends, cardio::cancellation cancellation) {
  co_await preparationCheckpoint(cancellation);
  const auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    const auto selection =
        selectDspBackend(DspBackendKind::scalar,
                         DspSimdVariant::automatic, backends);
    co_return prepareBypass(startupBuildOptions(options, {}), {},
                         configured.error, std::move(backends), selection);
  }
  const auto &config = configured.config;
  const auto startupOptions = startupBuildOptions(options, config);
  const auto selection =
      selectDspBackend(config.dspBackend, config.dspSimdVariant, backends);
  if (!config.presetFound) {
    co_return prepareBypass(startupOptions, config, {},
                         std::move(backends), selection);
  }
  if (selection.effectiveBackend == nullptr) {
    co_return prepareBypass(
        startupOptions, config,
        "cannot load configured preset: " + selection.error,
        std::move(backends), selection);
  }

  auto loaded = std::move(co_await loadDspPipelineAsync(config.presetPath, startupOptions,
      selection.effectiveBackend, defaultPipelineLoadContext(), cancellation));
  if (loaded.pipeline == nullptr) {
    co_return prepareBypass(
        startupOptions, config,
        "cannot load configured preset: " + loaded.error,
        std::move(backends), selection);
  }
  co_return StartupPipelineResult{.pipeline = std::move(loaded.pipeline),
          .activePresetPath = config.presetPath,
          .ratePolicy = config.ratePolicy,
          .dspIdlePolicy = config.dspIdlePolicy,
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
          .dspBackendError = selection.error,
          .outputConfiguration = config.outputConfiguration};
}

} // namespace pipetune
