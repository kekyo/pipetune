/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_DSP_PIPELINE_H
#define PIPETUNE_DSP_PIPELINE_H

#include "pipetune/dsp_backend.h"
#include "pipetune/asset_cache.h"
#include "pipetune/asset_memory.h"
#include "pipetune/preset_entry.h"
#include <cardio.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pipetune {

/**
 * Describes the maximum audio format for a native DSP pipeline.
 */
struct PipelineBuildOptions {
  /** Processing sample rate in hertz, from 32000 through 384000.
   * Individual DSPs may support only a subset and reject preparation.
   */
  float sampleRate;
  /** Maximum planar channel count, from one through sixteen. */
  std::uint32_t maxChannels;
  /** Maximum frame count accepted by one process call; must be at least 32. */
  std::uint32_t maxFrames;
};

/**
 * Reports a non-fatal condition encountered while preparing a preset node.
 */
struct PipelineWarning {
  /** Zero-based node index in the preset pipeline. */
  std::size_t nodeIndex;
  /** Display name stored in the preset. */
  std::string pluginName;
  /** Human-readable preparation or omission diagnostic. */
  std::string reason;
  /** Effective node state; enabled diagnostics do not mean omission. */
  PresetEntryState state = PresetEntryState::ignored;
};

/**
 * Status returned by the real-time DSP processing entry point.
 */
enum class ProcessStatus {
  /** The audio buffer was processed successfully. */
  ok,
  /** The buffer shape or timestamp is outside the prepared limits. */
  invalidBuffer,
  /** EffeTune's native DSP engine rejected the processing operation. */
  dspError
};

/**
 * Reports cumulative native EffeTune processing work.
 */
struct DspPerformanceCounters {
  /** Frames passed to the native EffeTune engine. */
  std::uint64_t processedFrames;
  /** Nanoseconds spent inside native EffeTune pipeline processing. */
  std::uint64_t processingNanoseconds;
};

class DspPipeline;
class DspPipelineSlot;
struct PipelineCreateResult;
struct PipelineLoadResult;

/** Non-audio resources used when preparing a preset. */
struct PipelineLoadContext {
  /** Directory containing EffeTune's measurement backup JSON files. */
  std::filesystem::path measurementDirectory;
  /** EffeTune data root containing registered IR and SFZ sources. */
  std::filesystem::path effetuneDirectory = {};
  /** Optional native preparation cache; a write failure does not prevent playback. */
  AssetCacheOptions assetCache = {};
  /** Combined asset working data and live native footprints; may lower the process default. */
  std::uint64_t assetMemoryBytes = kDefaultAssetMemoryBytes;
  /** Per-instrument original, PCM, and bank/index budget; defaults to the app's 256 MiB. */
  std::uint64_t sfzMaximumBytes = std::uint64_t{kDefaultSfzMaxSizeMiB} * 1024 * 1024;
};

/**
 * Resolves measurement storage from XDG_CONFIG_HOME and HOME.
 * @param sfzMaxSizeMiB Supported per-instrument SFZ budget in MiB.
 * @return Context with an empty directory when neither variable is available.
 * @throws std::invalid_argument When the SFZ budget is unsupported.
 */
PipelineLoadContext defaultPipelineLoadContext(std::uint32_t sfzMaxSizeMiB = kDefaultSfzMaxSizeMiB);

/**
 * Owns one prepared EffeTune native DSP pipeline.
 *
 * The object is built away from the audio thread. process() performs no
 * allocation and accepts channel-major planar floating-point PCM.
 */
class DspPipeline final {
  struct Impl;

public:
  /** Releases all native DSP instances and their engine. */
  ~DspPipeline();
  /** Transfers ownership of another pipeline. */
  DspPipeline(DspPipeline &&other) noexcept;
  /** Replaces this pipeline by transferring ownership from another pipeline. */
  DspPipeline &operator=(DspPipeline &&other) noexcept;
  /** Pipelines cannot be copied. */
  DspPipeline(const DspPipeline &) = delete;
  /** Pipelines cannot be copy-assigned. */
  DspPipeline &operator=(const DspPipeline &) = delete;

  /**
   * Processes channel-major planar PCM in place.
   *
   * @param planarSamples Contiguous channel-major samples.
   * @param channelCount Number of channels represented by the buffer.
   * @param frameCount Number of frames in each channel.
   * @param timeSeconds Monotonic stream time in seconds.
   * @return Processing status. On failure, the input buffer is left unchanged.
   */
  ProcessStatus process(std::span<float> planarSamples, std::uint32_t channelCount,
                        std::uint32_t frameCount, double timeSeconds) noexcept;

  /**
   * Clears retained DSP state while preserving the prepared pipeline.
   *
   * Filter, delay, tail, source-generator, and telemetry histories are reset.
   * The pipeline layout and configured parameters remain available for the
   * next process() call. This function performs no allocation.
   *
   * @return Processing status from the native EffeTune engine.
   */
  ProcessStatus reset() noexcept;

  /** Returns the sample rate supplied at construction. */
  float sampleRate() const noexcept;
  /** Returns the maximum channel count supplied at construction. */
  std::uint32_t maxChannels() const noexcept;
  /** Returns the maximum frame count supplied at construction. */
  std::uint32_t maxFrames() const noexcept;
  /** Returns the effective latency of output bus zero in frames. */
  std::uint32_t latencyFrames() const noexcept;
  /** Returns the number of enabled, supported native DSP nodes. */
  std::size_t activePluginCount() const noexcept;
  /** Returns all loaded preset entries in their original order. */
  std::span<const PresetEntry> presetEntries() const noexcept;
  /** Returns referenced asset source files, including missing files. */
  std::span<const std::filesystem::path> dependencyFiles() const noexcept;
  /** Returns the native backend in use, or no value for a bypass pipeline. */
  std::optional<DspBackendKind> backendKind() const noexcept;
  /** Returns the concrete native variant, or no value for a bypass pipeline. */
  std::optional<DspBackendVariant> backendVariant() const noexcept;

private:
  explicit DspPipeline(std::unique_ptr<Impl> implementation);
  static cardio::promise<PipelineLoadResult>
  buildFromRecipe(std::shared_ptr<const std::string> presetRecipe,
                  PipelineBuildOptions options,
                  std::shared_ptr<const DspBackend> backend,
                  PipelineLoadContext context, cardio::cancellation cancellation);
  bool usesNativeDsp() const noexcept;
  std::unique_ptr<Impl> implementation_;

  friend class DspPipelineSlot;
  friend cardio::promise<PipelineLoadResult> loadDspPipelineAsync(
      std::filesystem::path, PipelineBuildOptions, std::shared_ptr<const DspBackend>,
      PipelineLoadContext, cardio::cancellation);
  friend cardio::promise<PipelineLoadResult> rebuildDspPipelineAsync(
      const DspPipeline &, PipelineBuildOptions, std::shared_ptr<const DspBackend>, cardio::cancellation);
  friend struct PipelineCreateResult;
  friend struct PipelineLoadResult;
  friend PipelineCreateResult
  createBypassDspPipeline(const PipelineBuildOptions &options);
  friend PipelineLoadResult loadDspPipeline(const std::filesystem::path &presetPath,
                                            const PipelineBuildOptions &options,
                                            const PipelineLoadContext &context);
  friend PipelineLoadResult
  loadDspPipeline(const std::filesystem::path &presetPath,
                  const PipelineBuildOptions &options,
                  std::shared_ptr<const DspBackend> backend,
                  const PipelineLoadContext &context);
  friend PipelineLoadResult
  rebuildDspPipeline(const DspPipeline &source,
                     const PipelineBuildOptions &options);
  friend PipelineLoadResult
  rebuildDspPipeline(const DspPipeline &source,
                     const PipelineBuildOptions &options,
                     std::shared_ptr<const DspBackend> backend);
};

/**
 * Result of preparing a pipeline that does not invoke a DSP engine.
 */
struct PipelineCreateResult {
  /** Prepared bypass pipeline, or null when error is non-empty. */
  std::unique_ptr<DspPipeline> pipeline;
  /** Fatal construction diagnostic. */
  std::string error;
};

/**
 * Prepares a transparent pipeline without constructing an EffeTune engine.
 *
 * The returned pipeline validates process buffers against options and leaves
 * valid PCM samples unchanged.
 *
 * @param options Maximum processing format for the prepared pipeline.
 * @return A bypass pipeline or a fatal diagnostic.
 */
PipelineCreateResult
createBypassDspPipeline(const PipelineBuildOptions &options);

/**
 * Result of parsing a preset and preparing its native DSP pipeline.
 */
struct PipelineLoadResult {
  /** Prepared pipeline, or null when error is non-empty. */
  std::unique_ptr<DspPipeline> pipeline;
  /** Non-fatal preparation diagnostics with their effective node states. */
  std::vector<PipelineWarning> warnings;
  /** Referenced source paths, including unavailable inputs. */
  std::vector<std::filesystem::path> dependencyFiles = {};
  /** Number of external assets whose preparation was reused during this load. */
  std::size_t cachedAssetCount = 0;
  /** Fatal load or construction diagnostic. */
  std::string error;
};

/**
 * Loads a formal `.effetune_preset` file and prepares its supported native DSPs.
 *
 * Unknown DSPs and unresolved stored-asset DSPs are omitted with warnings.
 * Supported generated FIR assets are rebuilt at the requested sample rate.
 * Disabled nodes, including nodes gated by a disabled Section, are omitted
 * without warnings. Visualization-only analyzers are also omitted without
 * warnings, active nodes, added latency, or transfers between buses.
 * This includes Pitch Meter and Chroma Spiral. Bass Extender requires one or
 * two selected channels. Bass Management requires explicit All selection and
 * valid subwoofer routes; its Linear filters are generated from the preset.
 * The selected processing width is preserved for
 * Spatial Mapper; presets do not enlarge the prepared stream's channel count.
 *
 * @param context Measurement storage used during preparation and rebuilds.
 * @param presetPath Preset path with the exact `.effetune_preset` extension.
 * @param options Maximum processing format for the prepared native engine.
 * @return A pipeline or a fatal diagnostic, plus any non-fatal warnings.
 */
PipelineLoadResult loadDspPipeline(const std::filesystem::path &presetPath,
                                   const PipelineBuildOptions &options,
                                   const PipelineLoadContext &context = defaultPipelineLoadContext());

/**
 * Loads a preset with an explicitly selected, validated DSP backend.
 *
 * @param context Measurement storage used during preparation and rebuilds.
 * @param presetPath Preset path with the exact `.effetune_preset` extension.
 * @param options Maximum processing format for the prepared native engine.
 * @param backend Backend whose library must outlive the prepared engine.
 * @return A pipeline or a fatal diagnostic, plus any non-fatal warnings.
 */
PipelineLoadResult
loadDspPipeline(const std::filesystem::path &presetPath,
                const PipelineBuildOptions &options,
                std::shared_ptr<const DspBackend> backend,
                const PipelineLoadContext &context = defaultPipelineLoadContext());

/**
 * Loads and prepares a preset on the current cardio GIO dispatcher.
 * @param presetPath Formal preset path; retained until preparation completes.
 * @param options Processing format captured at request time.
 * @param backend Native backend retained independently of the active pipeline.
 * @param context Source and cache configuration captured at request time.
 * @param cancellation Superseded request or shutdown notification.
 * @return Prepared replacement or diagnostic; the active pipeline is unaffected.
 * @throws cardio::canceled_exception When preparation is canceled.
 */
cardio::promise<PipelineLoadResult> loadDspPipelineAsync(
    std::filesystem::path presetPath, PipelineBuildOptions options,
    std::shared_ptr<const DspBackend> backend, PipelineLoadContext context,
    cardio::cancellation cancellation);

/**
 * Rebuilds the retained recipe asynchronously without retaining the source engine.
 * @param source Pipeline whose immutable recipe is captured before returning.
 * @param options New processing format.
 * @param backend Explicit backend, or null to retain the source backend.
 * @param cancellation Superseded request or shutdown notification.
 * @return Prepared replacement; the source may be retired after this call returns.
 * @throws cardio::canceled_exception When preparation is canceled.
 */
cardio::promise<PipelineLoadResult> rebuildDspPipelineAsync(
    const DspPipeline &source, PipelineBuildOptions options,
    std::shared_ptr<const DspBackend> backend, cardio::cancellation cancellation);

/**
 * Rebuilds a pipeline at another rate from its retained preset recipe.
 *
 * No preset file is reopened. A bypass source produces another bypass
 * pipeline, while a preset source reparses the immutable in-memory recipe.
 *
 * @param source Existing prepared preset or bypass pipeline.
 * @param options New maximum processing format.
 * @return Rebuilt pipeline and warnings, or a fatal diagnostic.
 */
PipelineLoadResult
rebuildDspPipeline(const DspPipeline &source,
                   const PipelineBuildOptions &options);

/**
 * Rebuilds a retained preset recipe with an explicitly selected backend.
 *
 * A successful rebuild creates fresh DSP state. The source pipeline and its
 * backend remain unchanged.
 *
 * @param source Existing prepared preset or bypass pipeline.
 * @param options New maximum processing format.
 * @param backend Backend to use for the rebuilt native pipeline.
 * @return Rebuilt pipeline and warnings, or a fatal diagnostic.
 */
PipelineLoadResult
rebuildDspPipeline(const DspPipeline &source,
                   const PipelineBuildOptions &options,
                   std::shared_ptr<const DspBackend> backend);

} // namespace pipetune

#endif
