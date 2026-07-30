#include "dsp_idle_controller.h"

#include <cmath>
#include <cstddef>
#include <limits>

namespace pipetune {

constexpr auto kInputSilenceSeconds = std::uint64_t{5};
constexpr auto kOutputQuietSeconds = std::uint64_t{1};
constexpr auto kConservativeQuietAmplitude = 3.162277660168379e-8F;

static std::uint64_t saturatingAdd(std::uint64_t left,
                                   std::uint64_t right) noexcept {
  const auto maximum = std::numeric_limits<std::uint64_t>::max();
  return left > maximum - right ? maximum : left + right;
}

template <typename Predicate>
static std::uint32_t trailingMatchingFrames(
    std::span<const float> planarSamples, std::uint32_t channelCount,
    std::uint32_t frameCount, Predicate matches) noexcept {
  if (channelCount == 0 || frameCount == 0 ||
      planarSamples.size() !=
          static_cast<std::size_t>(channelCount) * frameCount) {
    return 0;
  }
  auto trailing = std::uint32_t{0};
  for (auto offset = std::uint32_t{0}; offset < frameCount; ++offset) {
    const auto frame = frameCount - offset - 1;
    auto frameMatches = true;
    for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
      if (!matches(planarSamples[static_cast<std::size_t>(channel) *
                                     frameCount +
                                 frame])) {
        frameMatches = false;
        break;
      }
    }
    if (!frameMatches) {
      break;
    }
    ++trailing;
  }
  return trailing;
}

DspIdleController::DspIdleController(
    std::uint32_t sampleRate, DspIdlePolicy policy) noexcept
    : sampleRate_(sampleRate == 0 ? 1 : sampleRate), policy_(policy),
      state_(DspIdleState::active), inputSilentFrames_(0),
      outputQuietFrames_(0), skippedFrames_(0), sleepTransitions_(0),
      sleepRequested_(false), resetFailed_(false) {}

bool DspIdleController::observeInput(
    std::span<const float> planarSamples, std::uint32_t channelCount,
    std::uint32_t frameCount) noexcept {
  sleepRequested_ = false;
  const auto expectedSamples =
      static_cast<std::size_t>(channelCount) * frameCount;
  if (channelCount == 0 || frameCount == 0 ||
      planarSamples.size() != expectedSamples) {
    state_ = DspIdleState::active;
    inputSilentFrames_ = 0;
    outputQuietFrames_ = 0;
    resetFailed_ = false;
    return true;
  }

  const auto trailingZero = trailingMatchingFrames(
      planarSamples, channelCount, frameCount,
      [](float sample) noexcept { return sample == 0.0F; });
  if (trailingZero != frameCount) {
    inputSilentFrames_ = trailingZero;
    outputQuietFrames_ = 0;
    resetFailed_ = false;
    state_ =
        trailingZero == 0 ? DspIdleState::active : DspIdleState::draining;
    return true;
  }

  inputSilentFrames_ =
      saturatingAdd(inputSilentFrames_, frameCount);
  if (state_ == DspIdleState::sleeping) {
    skippedFrames_ = saturatingAdd(skippedFrames_, frameCount);
    return false;
  }
  state_ = DspIdleState::draining;
  return true;
}

bool DspIdleController::observeOutput(
    std::span<const float> planarSamples, std::uint32_t channelCount,
    std::uint32_t frameCount, bool valid) noexcept {
  sleepRequested_ = false;
  const auto expectedSamples =
      static_cast<std::size_t>(channelCount) * frameCount;
  if (!valid || state_ == DspIdleState::sleeping || channelCount == 0 ||
      frameCount == 0 || planarSamples.size() != expectedSamples) {
    outputQuietFrames_ = 0;
    return false;
  }

  const auto trailingQuiet =
      policy_ == DspIdlePolicy::exact
          ? trailingMatchingFrames(
                planarSamples, channelCount, frameCount,
                [](float sample) noexcept { return sample == 0.0F; })
          : trailingMatchingFrames(
                planarSamples, channelCount, frameCount,
                [](float sample) noexcept {
                  return std::isfinite(sample) &&
                         std::abs(sample) <= kConservativeQuietAmplitude;
                });
  outputQuietFrames_ =
      trailingQuiet == frameCount
          ? saturatingAdd(outputQuietFrames_, frameCount)
          : trailingQuiet;
  const auto inputRequired =
      static_cast<std::uint64_t>(sampleRate_) * kInputSilenceSeconds;
  const auto outputRequired =
      static_cast<std::uint64_t>(sampleRate_) * kOutputQuietSeconds;
  sleepRequested_ = !resetFailed_ &&
                    inputSilentFrames_ >= inputRequired &&
                    outputQuietFrames_ >= outputRequired;
  return sleepRequested_;
}

void DspIdleController::enterSleep() noexcept {
  if (!sleepRequested_ || resetFailed_) {
    return;
  }
  state_ = DspIdleState::sleeping;
  sleepRequested_ = false;
  sleepTransitions_ = saturatingAdd(sleepTransitions_, 1);
}

void DspIdleController::rejectSleep() noexcept {
  sleepRequested_ = false;
  resetFailed_ = true;
  state_ = DspIdleState::draining;
}

void DspIdleController::restart(
    std::uint32_t sampleRate, DspIdlePolicy policy) noexcept {
  sampleRate_ = sampleRate == 0 ? 1 : sampleRate;
  policy_ = policy;
  state_ = DspIdleState::active;
  inputSilentFrames_ = 0;
  outputQuietFrames_ = 0;
  sleepRequested_ = false;
  resetFailed_ = false;
}

DspIdlePolicy DspIdleController::policy() const noexcept {
  return policy_;
}

DspIdleState DspIdleController::state() const noexcept {
  return state_;
}

std::uint64_t DspIdleController::inputSilentFrames() const noexcept {
  return inputSilentFrames_;
}

std::uint64_t DspIdleController::outputQuietFrames() const noexcept {
  return outputQuietFrames_;
}

std::uint64_t DspIdleController::skippedFrames() const noexcept {
  return skippedFrames_;
}

std::uint64_t DspIdleController::sleepTransitions() const noexcept {
  return sleepTransitions_;
}

} // namespace pipetune
