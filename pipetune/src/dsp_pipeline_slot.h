#ifndef PIPETUNE_DSP_PIPELINE_SLOT_H
#define PIPETUNE_DSP_PIPELINE_SLOT_H

#include "pipetune/dsp_pipeline.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pipetune {

/**
 * Owns one active DSP pipeline that a non-real-time thread may replace.
 *
 * Exactly one real-time thread may call process(). Replacement and destruction
 * must occur on one non-real-time thread. Superseded pipelines are reclaimed
 * only after the process callback no longer protects them.
 */
class DspPipelineSlot final {
public:
  /**
   * Creates a slot containing a prepared pipeline.
   *
   * @param initialPipeline Initial non-null pipeline.
   * @throws std::invalid_argument when initialPipeline is null.
   */
  explicit DspPipelineSlot(std::unique_ptr<DspPipeline> initialPipeline);

  /**
   * Releases active and superseded pipelines.
   *
   * No process() call may overlap destruction.
   */
  ~DspPipelineSlot();

  /** Slots cannot be copied. */
  DspPipelineSlot(const DspPipelineSlot &) = delete;
  /** Slots cannot be copy-assigned. */
  DspPipelineSlot &operator=(const DspPipelineSlot &) = delete;

  /**
   * Processes PCM with one complete active pipeline.
   *
   * This function performs no allocation, locking, or object destruction.
   *
   * @param planarSamples Contiguous channel-major samples.
   * @param channelCount Number of channels represented by the buffer.
   * @param frameCount Number of frames in each channel.
   * @param timeSeconds Monotonic stream time in seconds.
   * @return Processing status from the selected pipeline.
   */
  ProcessStatus process(std::span<float> planarSamples,
                        std::uint32_t channelCount, std::uint32_t frameCount,
                        double timeSeconds) noexcept;

  /**
   * Atomically activates a prepared replacement.
   *
   * Superseded objects are reclaimed here when the process callback has
   * released them; reclamation never occurs in process().
   *
   * @param replacement New non-null pipeline.
   * @throws std::invalid_argument when replacement is null.
   */
  void replace(std::unique_ptr<DspPipeline> replacement);

  /** Returns the active pipeline's enabled native DSP count. */
  std::size_t activePluginCount() const noexcept;

private:
  void reclaimSuperseded();

  std::unique_ptr<DspPipeline> current_;
  std::vector<std::unique_ptr<DspPipeline>> superseded_;
  std::atomic<DspPipeline *> active_;
  std::atomic<DspPipeline *> hazard_;
};

} // namespace pipetune

#endif
