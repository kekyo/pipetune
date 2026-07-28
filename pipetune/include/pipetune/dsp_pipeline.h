#ifndef PIPETUNE_DSP_PIPELINE_H
#define PIPETUNE_DSP_PIPELINE_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace pipetune {

/**
 * Describes the maximum audio format for a native DSP pipeline.
 */
struct PipelineBuildOptions {
  /** Processing sample rate in hertz, from 32000 through 192000. */
  float sampleRate;
  /** Maximum planar channel count, from one through eight. */
  std::uint32_t maxChannels;
  /** Maximum frame count accepted by one process call; must be at least 32. */
  std::uint32_t maxFrames;
};

/**
 * Reports a preset node that was intentionally omitted from the native pipeline.
 */
struct PipelineWarning {
  /** Zero-based node index in the preset pipeline. */
  std::size_t nodeIndex;
  /** Display name stored in the preset. */
  std::string pluginName;
  /** Human-readable reason for omitting the node. */
  std::string reason;
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

private:
  explicit DspPipeline(std::unique_ptr<Impl> implementation);
  bool usesNativeDsp() const noexcept;
  std::unique_ptr<Impl> implementation_;

  friend class DspPipelineSlot;
  friend struct PipelineCreateResult;
  friend struct PipelineLoadResult;
  friend PipelineCreateResult
  createBypassDspPipeline(const PipelineBuildOptions &options);
  friend PipelineLoadResult loadDspPipeline(const std::filesystem::path &presetPath,
                                            const PipelineBuildOptions &options);
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
  /** Non-fatal omitted-node diagnostics. */
  std::vector<PipelineWarning> warnings;
  /** Fatal load or construction diagnostic. */
  std::string error;
};

/**
 * Loads a formal `.effetune_preset` file and prepares its supported native DSPs.
 *
 * Unknown DSPs and DSPs requiring external assets are omitted with warnings.
 * Disabled nodes, including nodes gated by a disabled Section, are omitted
 * without warnings.
 *
 * @param presetPath Preset path with the exact `.effetune_preset` extension.
 * @param options Maximum processing format for the prepared native engine.
 * @return A pipeline or a fatal diagnostic, plus any non-fatal warnings.
 */
PipelineLoadResult loadDspPipeline(const std::filesystem::path &presetPath,
                                   const PipelineBuildOptions &options);

} // namespace pipetune

#endif
