#include "dsp_pipeline_slot.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

namespace pipetune {

DspPipelineSlot::DspPipelineSlot(
    std::unique_ptr<DspPipeline> initialPipeline)
    : current_(std::move(initialPipeline)), stagedPrevious_(nullptr),
      superseded_(),
      active_(current_.get()), hazard_(nullptr), processedFrames_(0),
      processingNanoseconds_(0) {
  if (current_ == nullptr) {
    throw std::invalid_argument("initial DSP pipeline must not be null");
  }
}

DspPipelineSlot::~DspPipelineSlot() = default;

ProcessStatus DspPipelineSlot::process(std::span<float> planarSamples,
                                       std::uint32_t channelCount,
                                       std::uint32_t frameCount,
                                       double timeSeconds) noexcept {
  auto *selected = active_.load(std::memory_order_seq_cst);
  do {
    hazard_.store(selected, std::memory_order_seq_cst);
    selected = active_.load(std::memory_order_seq_cst);
  } while (hazard_.load(std::memory_order_seq_cst) != selected);

  const auto usesNativeDsp = selected->usesNativeDsp();
  const auto startedAt =
      usesNativeDsp ? std::chrono::steady_clock::now()
                    : std::chrono::steady_clock::time_point{};
  const auto status =
      selected->process(planarSamples, channelCount, frameCount, timeSeconds);
  if (usesNativeDsp) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - startedAt);
    processedFrames_.fetch_add(frameCount, std::memory_order_relaxed);
    processingNanoseconds_.fetch_add(
        static_cast<std::uint64_t>(elapsed.count()),
        std::memory_order_relaxed);
  }
  hazard_.store(nullptr, std::memory_order_seq_cst);
  return status;
}

void DspPipelineSlot::replace(std::unique_ptr<DspPipeline> replacement) {
  stageReplacement(std::move(replacement));
  commitStaged();
}

PipelineLoadResult DspPipelineSlot::rebuildActive(
    const PipelineBuildOptions &options) const {
  return rebuildDspPipeline(*current_, options);
}

PipelineLoadResult DspPipelineSlot::rebuildActive(
    const PipelineBuildOptions &options,
    std::shared_ptr<const DspBackend> backend) const {
  return rebuildDspPipeline(*current_, options, std::move(backend));
}

void DspPipelineSlot::stageReplacement(
    std::unique_ptr<DspPipeline> replacement) {
  if (replacement == nullptr) {
    throw std::invalid_argument("replacement DSP pipeline must not be null");
  }
  if (stagedPrevious_ != nullptr) {
    throw std::logic_error("a DSP pipeline replacement is already staged");
  }

  stagedPrevious_ = std::move(current_);
  current_ = std::move(replacement);
  active_.store(current_.get(), std::memory_order_seq_cst);
}

void DspPipelineSlot::commitStaged() {
  if (stagedPrevious_ == nullptr) {
    throw std::logic_error("no DSP pipeline replacement is staged");
  }
  superseded_.push_back(std::move(stagedPrevious_));
  reclaimSuperseded();
}

void DspPipelineSlot::rollbackStaged() {
  if (stagedPrevious_ == nullptr) {
    throw std::logic_error("no DSP pipeline replacement is staged");
  }
  auto rejected = std::move(current_);
  current_ = std::move(stagedPrevious_);
  active_.store(current_.get(), std::memory_order_seq_cst);
  superseded_.push_back(std::move(rejected));
  reclaimSuperseded();
}

bool DspPipelineSlot::hasStagedReplacement() const noexcept {
  return stagedPrevious_ != nullptr;
}

std::size_t DspPipelineSlot::activePluginCount() const noexcept {
  return active_.load(std::memory_order_acquire)->activePluginCount();
}

std::optional<DspBackendKind>
DspPipelineSlot::backendKind() const noexcept {
  return active_.load(std::memory_order_acquire)->backendKind();
}

DspPerformanceCounters
DspPipelineSlot::performanceCounters() const noexcept {
  return {
      .processedFrames =
          processedFrames_.load(std::memory_order_relaxed),
      .processingNanoseconds =
          processingNanoseconds_.load(std::memory_order_relaxed),
  };
}

void DspPipelineSlot::reclaimSuperseded() {
  const auto *protectedPipeline = hazard_.load(std::memory_order_seq_cst);
  std::erase_if(superseded_, [protectedPipeline](const auto &pipeline) {
    return pipeline.get() != protectedPipeline;
  });
}

} // namespace pipetune
