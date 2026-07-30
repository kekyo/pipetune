#include "pipewire_volume_smoother.h"

#include <spa/param/props.h>
#include <spa/pod/iter.h>

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace pipetune {

static_assert(std::atomic<float>::is_always_lock_free);

static bool readVolume(const spa_pod_prop *property, float &destination) {
  auto value = 0.0F;
  if (property == nullptr ||
      spa_pod_get_float(&property->value, &value) < 0 ||
      !std::isfinite(value) || value < 0.0F) {
    return false;
  }
  destination = value;
  return true;
}

static bool readMute(const spa_pod_prop *property, bool &destination) {
  auto value = false;
  if (property == nullptr ||
      spa_pod_get_bool(&property->value, &value) < 0) {
    return false;
  }
  destination = value;
  return true;
}

PipeWireVolumeSmoother::PipeWireVolumeSmoother(
    std::uint32_t channelCount) noexcept
    : channelCount_(std::min(channelCount, kMaximumChannels)),
      masterVolume_(1.0F), channelVolumes_{}, muted_(false),
      publicationSequence_(0), publishedGains_{},
      publishedRampSamples_(1), appliedSequence_(0), currentGains_{},
      targetGains_{}, gainSteps_{}, remainingRampSamples_(0) {
  channelVolumes_.fill(1.0F);
  currentGains_.fill(1.0F);
  targetGains_.fill(1.0F);
  gainSteps_.fill(0.0F);
  for (auto &gain : publishedGains_) {
    gain.store(1.0F, std::memory_order_relaxed);
  }
}

bool PipeWireVolumeSmoother::update(
    const spa_pod *parameter, std::uint32_t rampSamples) noexcept {
  if (parameter == nullptr || rampSamples == 0 ||
      !spa_pod_is_object_type(parameter, SPA_TYPE_OBJECT_Props)) {
    return false;
  }

  auto changed = false;
  auto updatedMaster = masterVolume_;
  if (readVolume(
          spa_pod_find_prop(parameter, nullptr, SPA_PROP_volume),
          updatedMaster) &&
      updatedMaster != masterVolume_) {
    masterVolume_ = updatedMaster;
    changed = true;
  }

  auto updatedMute = muted_;
  auto *muteProperty =
      spa_pod_find_prop(parameter, nullptr, SPA_PROP_mute);
  if (muteProperty == nullptr) {
    muteProperty =
        spa_pod_find_prop(parameter, nullptr, SPA_PROP_softMute);
  }
  if (readMute(muteProperty, updatedMute) &&
      updatedMute != muted_) {
    muted_ = updatedMute;
    changed = true;
  }

  auto *volumeProperty =
      spa_pod_find_prop(parameter, nullptr, SPA_PROP_channelVolumes);
  if (volumeProperty == nullptr) {
    volumeProperty =
        spa_pod_find_prop(parameter, nullptr, SPA_PROP_softVolumes);
  }
  auto volumes = std::array<float, kMaximumChannels>{};
  const auto volumeCount =
      volumeProperty == nullptr
          ? std::uint32_t{0}
          : spa_pod_copy_array(
                &volumeProperty->value, SPA_TYPE_Float, volumes.data(),
                channelCount_);
  if (volumeCount != 0) {
    for (auto channel = std::uint32_t{0}; channel < channelCount_;
         ++channel) {
      const auto source =
          volumeCount == 1 ? std::uint32_t{0}
                           : std::min(channel, volumeCount - 1);
      const auto volume = volumes[source];
      if (!std::isfinite(volume) || volume < 0.0F) {
        continue;
      }
      if (channelVolumes_[channel] != volume) {
        channelVolumes_[channel] = volume;
        changed = true;
      }
    }
  }
  if (!changed) {
    return false;
  }

  publicationSequence_.fetch_add(1, std::memory_order_acq_rel);
  for (auto channel = std::uint32_t{0}; channel < channelCount_;
       ++channel) {
    const auto gain =
        muted_ ? 0.0F : masterVolume_ * channelVolumes_[channel];
    publishedGains_[channel].store(gain, std::memory_order_relaxed);
  }
  publishedRampSamples_.store(rampSamples, std::memory_order_relaxed);
  publicationSequence_.fetch_add(1, std::memory_order_release);
  return true;
}

void PipeWireVolumeSmoother::process(
    std::span<float> planarSamples,
    std::uint32_t frameCount) noexcept {
  if (frameCount == 0 || channelCount_ == 0 ||
      planarSamples.size() <
          static_cast<std::size_t>(channelCount_) * frameCount) {
    return;
  }

  consumePublishedUpdate();

  const auto transitionFrames =
      std::min(frameCount, remainingRampSamples_);
  for (auto channel = std::uint32_t{0}; channel < channelCount_;
       ++channel) {
    auto samples = planarSamples.subspan(
        static_cast<std::size_t>(channel) * frameCount, frameCount);
    auto gain = currentGains_[channel];
    for (auto frame = std::uint32_t{0}; frame < transitionFrames;
         ++frame) {
      gain += gainSteps_[channel];
      samples[frame] *= gain;
    }
    if (transitionFrames == remainingRampSamples_) {
      gain = targetGains_[channel];
    }
    currentGains_[channel] = gain;
    if (gain == 1.0F) {
      continue;
    }
    if (gain == 0.0F) {
      std::fill(samples.begin() + transitionFrames, samples.end(),
                0.0F);
      continue;
    }
    for (auto frame = transitionFrames; frame < frameCount; ++frame) {
      samples[frame] *= gain;
    }
  }
  remainingRampSamples_ -= transitionFrames;
  if (remainingRampSamples_ == 0) {
    for (auto channel = std::uint32_t{0}; channel < channelCount_;
         ++channel) {
      currentGains_[channel] = targetGains_[channel];
    }
  }
}

void PipeWireVolumeSmoother::advance(
    std::uint32_t frameCount) noexcept {
  if (frameCount == 0 || channelCount_ == 0) {
    return;
  }
  consumePublishedUpdate();
  const auto transitionFrames =
      std::min(frameCount, remainingRampSamples_);
  for (auto channel = std::uint32_t{0}; channel < channelCount_;
       ++channel) {
    auto gain = currentGains_[channel];
    for (auto frame = std::uint32_t{0}; frame < transitionFrames;
         ++frame) {
      gain += gainSteps_[channel];
    }
    currentGains_[channel] =
        transitionFrames == remainingRampSamples_
            ? targetGains_[channel]
            : gain;
  }
  remainingRampSamples_ -= transitionFrames;
}

void PipeWireVolumeSmoother::consumePublishedUpdate() noexcept {
  const auto sequence =
      publicationSequence_.load(std::memory_order_acquire);
  if ((sequence & 1U) == 0 && sequence != appliedSequence_) {
    auto published = std::array<float, kMaximumChannels>{};
    for (auto channel = std::uint32_t{0}; channel < channelCount_;
         ++channel) {
      published[channel] =
          publishedGains_[channel].load(std::memory_order_relaxed);
    }
    const auto rampSamples =
        publishedRampSamples_.load(std::memory_order_relaxed);
    if (publicationSequence_.load(std::memory_order_acquire) ==
        sequence) {
      appliedSequence_ = sequence;
      remainingRampSamples_ = std::max(std::uint32_t{1}, rampSamples);
      for (auto channel = std::uint32_t{0};
           channel < channelCount_; ++channel) {
        targetGains_[channel] = published[channel];
        gainSteps_[channel] =
            (targetGains_[channel] - currentGains_[channel]) /
            static_cast<float>(remainingRampSamples_);
      }
    }
  }
}

} // namespace pipetune
