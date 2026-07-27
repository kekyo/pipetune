#include "dsp_pipeline_slot.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace pipetune {

DspPipelineSlot::DspPipelineSlot(
    std::unique_ptr<DspPipeline> initialPipeline)
    : current_(std::move(initialPipeline)), superseded_(),
      active_(current_.get()), hazard_(nullptr) {
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

  const auto status =
      selected->process(planarSamples, channelCount, frameCount, timeSeconds);
  hazard_.store(nullptr, std::memory_order_seq_cst);
  return status;
}

void DspPipelineSlot::replace(std::unique_ptr<DspPipeline> replacement) {
  if (replacement == nullptr) {
    throw std::invalid_argument("replacement DSP pipeline must not be null");
  }

  auto previous = std::move(current_);
  current_ = std::move(replacement);
  active_.store(current_.get(), std::memory_order_seq_cst);
  superseded_.push_back(std::move(previous));
  reclaimSuperseded();
}

std::size_t DspPipelineSlot::activePluginCount() const noexcept {
  return active_.load(std::memory_order_acquire)->activePluginCount();
}

void DspPipelineSlot::reclaimSuperseded() {
  const auto *protectedPipeline = hazard_.load(std::memory_order_seq_cst);
  std::erase_if(superseded_, [protectedPipeline](const auto &pipeline) {
    return pipeline.get() != protectedPipeline;
  });
}

} // namespace pipetune
