#include "audio_bridge.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace pipetune {

static void copyIntoRing(std::span<const float> source, std::span<float> destination,
                         std::uint64_t absoluteFrame, std::uint32_t frameCount) noexcept {
  const auto offset = static_cast<std::uint32_t>(absoluteFrame % destination.size());
  const auto firstCount =
      std::min(frameCount, static_cast<std::uint32_t>(destination.size()) - offset);
  std::copy_n(source.begin(), firstCount, destination.begin() + offset);
  std::copy_n(source.begin() + firstCount, frameCount - firstCount, destination.begin());
}

static void copyFromRing(std::span<const float> source, std::span<float> destination,
                         std::uint64_t absoluteFrame, std::uint32_t frameCount) noexcept {
  const auto offset = static_cast<std::uint32_t>(absoluteFrame % source.size());
  const auto firstCount =
      std::min(frameCount, static_cast<std::uint32_t>(source.size()) - offset);
  std::copy_n(source.begin() + offset, firstCount, destination.begin());
  std::copy_n(source.begin(), frameCount - firstCount, destination.begin() + firstCount);
}

PlanarAudioRing::PlanarAudioRing(std::uint32_t channelCount, std::uint32_t capacityFrames)
    : channelCount_(channelCount), capacityFrames_(capacityFrames),
      samples_(static_cast<std::size_t>(channelCount) * capacityFrames, 0.0F), readFrame_(0),
      writeFrame_(0), overrunFrames_(0), underrunFrames_(0) {
  if (channelCount == 0 || channelCount > 8) {
    throw std::invalid_argument("channel count must be between one and eight");
  }
  if (capacityFrames == 0) {
    throw std::invalid_argument("frame capacity must be positive");
  }
}

std::uint32_t PlanarAudioRing::write(std::span<const float> planarSamples,
                                     std::uint32_t frameCount) noexcept {
  if (planarSamples.size() != static_cast<std::size_t>(channelCount_) * frameCount) {
    return 0;
  }

  const auto writeFrame = writeFrame_.load(std::memory_order_relaxed);
  const auto readFrame = readFrame_.load(std::memory_order_acquire);
  const auto queuedFrames =
      std::min<std::uint64_t>(writeFrame - readFrame, capacityFrames_);
  const auto availableFrames =
      capacityFrames_ - static_cast<std::uint32_t>(queuedFrames);
  const auto acceptedFrames = std::min(frameCount, availableFrames);

  for (auto channel = std::uint32_t{0}; channel < channelCount_; ++channel) {
    const auto source = planarSamples.subspan(
        static_cast<std::size_t>(channel) * frameCount, acceptedFrames);
    auto destination = std::span<float>(samples_).subspan(
        static_cast<std::size_t>(channel) * capacityFrames_, capacityFrames_);
    copyIntoRing(source, destination, writeFrame, acceptedFrames);
  }
  writeFrame_.store(writeFrame + acceptedFrames, std::memory_order_release);
  overrunFrames_.fetch_add(frameCount - acceptedFrames, std::memory_order_relaxed);
  return acceptedFrames;
}

std::uint32_t PlanarAudioRing::read(std::span<float> planarSamples,
                                    std::uint32_t frameCount) noexcept {
  if (planarSamples.size() != static_cast<std::size_t>(channelCount_) * frameCount) {
    return 0;
  }

  const auto readFrame = readFrame_.load(std::memory_order_relaxed);
  const auto writeFrame = writeFrame_.load(std::memory_order_acquire);
  const auto queuedFrames =
      std::min<std::uint64_t>(writeFrame - readFrame, capacityFrames_);
  const auto copiedFrames =
      std::min(frameCount, static_cast<std::uint32_t>(queuedFrames));

  for (auto channel = std::uint32_t{0}; channel < channelCount_; ++channel) {
    const auto source = std::span<const float>(samples_).subspan(
        static_cast<std::size_t>(channel) * capacityFrames_, capacityFrames_);
    auto destination = planarSamples.subspan(
        static_cast<std::size_t>(channel) * frameCount, frameCount);
    copyFromRing(source, destination.first(copiedFrames), readFrame, copiedFrames);
    std::fill(destination.begin() + copiedFrames, destination.end(), 0.0F);
  }
  readFrame_.store(readFrame + copiedFrames, std::memory_order_release);
  underrunFrames_.fetch_add(frameCount - copiedFrames, std::memory_order_relaxed);
  return copiedFrames;
}

std::uint32_t PlanarAudioRing::channelCount() const noexcept {
  return channelCount_;
}

std::uint32_t PlanarAudioRing::capacityFrames() const noexcept {
  return capacityFrames_;
}

std::uint64_t PlanarAudioRing::overrunFrames() const noexcept {
  return overrunFrames_.load(std::memory_order_relaxed);
}

std::uint64_t PlanarAudioRing::underrunFrames() const noexcept {
  return underrunFrames_.load(std::memory_order_relaxed);
}

} // namespace pipetune
