/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_pipeline_slot.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <utility>

#if defined(__SSE__) || defined(_M_X64)
#include <immintrin.h>
#endif

namespace pipetune {

static void enableDspDenormalFlush() noexcept {
#if defined(__SSE__) || defined(_M_X64)
  // Recursive DSP can retain inaudible subnormal tails after input stops.
  // Configure only the current processing thread and preserve all other MXCSR
  // control and status bits.
  constexpr auto denormalModeMask = static_cast<unsigned int>(
      _MM_FLUSH_ZERO_MASK | _MM_DENORMALS_ZERO_MASK);
  const auto control = _mm_getcsr();
  if ((control & denormalModeMask) != denormalModeMask) {
    _mm_setcsr(control | denormalModeMask);
  }
#endif
}

DspPipelineSlot::DspPipelineSlot(
    std::unique_ptr<DspPipeline> initialPipeline)
    : current_(std::move(initialPipeline)), stagedPrevious_(nullptr),
      superseded_(),
      active_(current_.get()), hazard_(nullptr), activationSequence_(0),
      processedFrames_(0), processingNanoseconds_(0) {
  if (current_ == nullptr) {
    throw std::invalid_argument("initial DSP pipeline must not be null");
  }
}

DspPipelineSlot::~DspPipelineSlot() = default;

ProcessStatus DspPipelineSlot::process(std::span<float> planarSamples,
                                       std::uint32_t channelCount,
                                       std::uint32_t frameCount,
                                       double timeSeconds) noexcept {
  return processWithGeneration(planarSamples, channelCount, frameCount,
                               timeSeconds)
      .status;
}

DspPipelineProcessResult DspPipelineSlot::processWithGeneration(
    std::span<float> planarSamples, std::uint32_t channelCount,
    std::uint32_t frameCount, double timeSeconds) noexcept {
  auto sequence = std::uint64_t{0};
  auto *selected = static_cast<DspPipeline *>(nullptr);
  while (true) {
    sequence = activationSequence_.load(std::memory_order_seq_cst);
    if ((sequence & 1U) != 0) {
      continue;
    }
    selected = active_.load(std::memory_order_seq_cst);
    hazard_.store(selected, std::memory_order_seq_cst);
    if (activationSequence_.load(std::memory_order_seq_cst) == sequence &&
        active_.load(std::memory_order_seq_cst) == selected) {
      break;
    }
    hazard_.store(nullptr, std::memory_order_seq_cst);
  }

  const auto usesNativeDsp = selected->usesNativeDsp();
  if (usesNativeDsp) {
    enableDspDenormalFlush();
  }
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
  return {.status = status, .generation = sequence / 2};
}

std::uint64_t DspPipelineSlot::activeGeneration() const noexcept {
  while (true) {
    const auto sequence =
        activationSequence_.load(std::memory_order_seq_cst);
    if ((sequence & 1U) == 0) {
      return sequence / 2;
    }
  }
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
  activate(current_.get());
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
  activate(current_.get());
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

std::optional<DspBackendVariant>
DspPipelineSlot::backendVariant() const noexcept {
  return active_.load(std::memory_order_acquire)->backendVariant();
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

void DspPipelineSlot::activate(DspPipeline *pipeline) noexcept {
  // Odd sequences prevent readers from pairing either pointer with the wrong
  // generation. Each stable even sequence identifies one activation.
  activationSequence_.fetch_add(1, std::memory_order_seq_cst);
  active_.store(pipeline, std::memory_order_seq_cst);
  activationSequence_.fetch_add(1, std::memory_order_seq_cst);
}

void DspPipelineSlot::reclaimSuperseded() {
  const auto *protectedPipeline = hazard_.load(std::memory_order_seq_cst);
  std::erase_if(superseded_, [protectedPipeline](const auto &pipeline) {
    return pipeline.get() != protectedPipeline;
  });
}

} // namespace pipetune
