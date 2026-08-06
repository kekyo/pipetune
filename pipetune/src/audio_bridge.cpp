#include "audio_bridge.h"

#include <algorithm>
#include <cstddef>
#include <stdexcept>

namespace pipetune {

static void copyIntoRing(std::span<const float> source,
                         std::span<float> destination,
                         std::uint64_t absoluteFrame,
                         std::uint32_t frameCount) noexcept {
  const auto offset = static_cast<std::uint32_t>(absoluteFrame % destination.size());
  const auto firstCount =
      std::min(frameCount, static_cast<std::uint32_t>(destination.size()) - offset);
  std::copy_n(source.begin(), firstCount, destination.begin() + offset);
  std::copy_n(source.begin() + firstCount, frameCount - firstCount, destination.begin());
}

static void copyFromRing(std::span<const float> source,
                         std::span<float> destination,
                         std::uint64_t absoluteFrame,
                         std::uint32_t frameCount) noexcept {
  const auto offset = static_cast<std::uint32_t>(absoluteFrame % source.size());
  const auto firstCount =
      std::min(frameCount, static_cast<std::uint32_t>(source.size()) - offset);
  std::copy_n(source.begin() + offset, firstCount, destination.begin());
  std::copy_n(source.begin(), frameCount - firstCount, destination.begin() + firstCount);
}

PlanarAudioRing::PlanarAudioRing(std::uint32_t channelCount, std::uint32_t capacityFrames)
    : channelCount_(channelCount), capacityFrames_(capacityFrames),
      samples_(static_cast<std::size_t>(channelCount) * capacityFrames, 0.0F),
      generations_(capacityFrames, 0),
      readFrame_(0), writeFrame_(0), overrunFrames_(0),
      underrunFrames_(0) {
  if (channelCount == 0 || channelCount > 8) {
    throw std::invalid_argument("channel count must be between one and eight");
  }
  if (capacityFrames == 0) {
    throw std::invalid_argument("frame capacity must be positive");
  }
}

std::uint32_t PlanarAudioRing::write(std::span<const float> planarSamples,
                                     std::uint32_t frameCount,
                                     std::uint64_t generation) noexcept {
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
  for (auto frame = std::uint32_t{0}; frame < acceptedFrames; ++frame) {
    generations_[(writeFrame + frame) % capacityFrames_] = generation;
  }
  writeFrame_.store(writeFrame + acceptedFrames, std::memory_order_release);
  overrunFrames_.fetch_add(frameCount - acceptedFrames, std::memory_order_relaxed);
  return acceptedFrames;
}

std::uint32_t PlanarAudioRing::read(std::span<float> planarSamples,
                                    std::uint32_t frameCount,
                                    std::uint64_t expectedGeneration) noexcept {
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
  for (auto frame = std::uint32_t{0}; frame < copiedFrames; ++frame) {
    if (generations_[(readFrame + frame) % capacityFrames_] ==
        expectedGeneration) {
      continue;
    }
    for (auto channel = std::uint32_t{0}; channel < channelCount_; ++channel) {
      planarSamples[static_cast<std::size_t>(channel) * frameCount + frame] =
          0.0F;
    }
  }
  readFrame_.store(readFrame + copiedFrames, std::memory_order_release);
  underrunFrames_.fetch_add(frameCount - copiedFrames,
                            std::memory_order_relaxed);
  return copiedFrames;
}

std::uint32_t PlanarAudioRing::discardQueuedFrames() noexcept {
  const auto writeFrame = writeFrame_.load(std::memory_order_acquire);
  const auto readFrame =
      readFrame_.exchange(writeFrame, std::memory_order_acq_rel);
  return static_cast<std::uint32_t>(
      std::min<std::uint64_t>(writeFrame - readFrame, capacityFrames_));
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

AudioTransitionSilencer::AudioTransitionSilencer(
    std::uint64_t initialGeneration) noexcept
    : observedGeneration_(initialGeneration), lastOutputSamples_{},
      fadeStartSamples_{}, phase_(Phase::steady), remainingFrames_(0),
      pendingSilenceFrames_(0), fadeFrames_(0) {}

void AudioTransitionSilencer::beginTransition(
    std::uint32_t silenceFrames, std::uint32_t fadeFrames) noexcept {
  fadeStartSamples_ = lastOutputSamples_;
  pendingSilenceFrames_ = silenceFrames;
  fadeFrames_ = fadeFrames;
  if (fadeFrames != 0) {
    phase_ = Phase::fadingOut;
    remainingFrames_ = fadeFrames;
  } else if (silenceFrames != 0) {
    phase_ = Phase::silent;
    remainingFrames_ = silenceFrames;
    pendingSilenceFrames_ = 0;
  } else {
    phase_ = Phase::awaitingAudio;
    remainingFrames_ = 0;
  }
}

void AudioTransitionSilencer::start(std::uint32_t silenceFrames,
                                    std::uint32_t fadeFrames) noexcept {
  beginTransition(silenceFrames, fadeFrames);
}

void AudioTransitionSilencer::reset(std::uint32_t silenceFrames,
                                    std::uint32_t fadeFrames) noexcept {
  lastOutputSamples_.fill(0.0F);
  fadeStartSamples_.fill(0.0F);
  pendingSilenceFrames_ = 0;
  fadeFrames_ = fadeFrames;
  if (silenceFrames == 0) {
    phase_ = Phase::awaitingAudio;
    remainingFrames_ = 0;
  } else {
    phase_ = Phase::silent;
    remainingFrames_ = silenceFrames;
  }
}

std::uint32_t AudioTransitionSilencer::apply(
    std::span<float> planarSamples, std::uint32_t channelCount,
    std::uint32_t frameCount, std::uint32_t availableFrames,
    std::uint64_t generation, std::uint32_t silenceFrames,
    std::uint32_t fadeFrames) noexcept {
  if (channelCount == 0 || channelCount > lastOutputSamples_.size() ||
      availableFrames > frameCount ||
      planarSamples.size() !=
          static_cast<std::size_t>(channelCount) * frameCount) {
    return 0;
  }
  if (generation != observedGeneration_) {
    observedGeneration_ = generation;
    beginTransition(silenceFrames, fadeFrames);
  }

  auto adjustedFrames = std::uint32_t{0};
  for (auto frame = std::uint32_t{0}; frame < frameCount; ++frame) {
    const auto available = frame < availableFrames;
    auto adjusted = false;
    auto reconsiderPhase = true;
    while (reconsiderPhase) {
      reconsiderPhase = false;
      switch (phase_) {
      case Phase::steady:
        if (!available) {
          beginTransition(silenceFrames, fadeFrames);
          reconsiderPhase = true;
        }
        break;
      case Phase::fadingOut: {
        const auto gain = fadeFrames_ == 0
                              ? 0.0F
                              : static_cast<float>(remainingFrames_ - 1) /
                                    static_cast<float>(fadeFrames_);
        for (auto channel = std::uint32_t{0}; channel < channelCount;
             ++channel) {
          planarSamples[static_cast<std::size_t>(channel) * frameCount +
                        frame] = fadeStartSamples_[channel] * gain;
        }
        adjusted = true;
        --remainingFrames_;
        if (remainingFrames_ == 0) {
          if (pendingSilenceFrames_ == 0) {
            phase_ = Phase::awaitingAudio;
          } else {
            phase_ = Phase::silent;
            remainingFrames_ = pendingSilenceFrames_;
            pendingSilenceFrames_ = 0;
          }
        }
        break;
      }
      case Phase::silent:
        for (auto channel = std::uint32_t{0}; channel < channelCount;
             ++channel) {
          planarSamples[static_cast<std::size_t>(channel) * frameCount +
                        frame] = 0.0F;
        }
        adjusted = true;
        if (available) {
          --remainingFrames_;
          if (remainingFrames_ == 0) {
            phase_ = Phase::awaitingAudio;
          }
        }
        break;
      case Phase::awaitingAudio:
        if (available) {
          if (fadeFrames == 0) {
            phase_ = Phase::steady;
          } else {
            phase_ = Phase::fadingIn;
            fadeFrames_ = fadeFrames;
            remainingFrames_ = fadeFrames;
          }
          reconsiderPhase = true;
        } else {
          for (auto channel = std::uint32_t{0}; channel < channelCount;
               ++channel) {
            planarSamples[static_cast<std::size_t>(channel) * frameCount +
                          frame] = 0.0F;
          }
          adjusted = true;
        }
        break;
      case Phase::fadingIn:
        if (!available) {
          beginTransition(silenceFrames, fadeFrames);
          reconsiderPhase = true;
        } else {
          const auto gain =
              static_cast<float>(fadeFrames_ - remainingFrames_ + 1) /
              static_cast<float>(fadeFrames_);
          for (auto channel = std::uint32_t{0}; channel < channelCount;
               ++channel) {
            planarSamples[static_cast<std::size_t>(channel) * frameCount +
                          frame] *= gain;
          }
          adjusted = true;
          --remainingFrames_;
          if (remainingFrames_ == 0) {
            phase_ = Phase::steady;
          }
        }
        break;
      }
    }
    if (adjusted) {
      ++adjustedFrames;
    }
    for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
      lastOutputSamples_[channel] =
          planarSamples[static_cast<std::size_t>(channel) * frameCount +
                        frame];
    }
  }
  return adjustedFrames;
}

} // namespace pipetune
