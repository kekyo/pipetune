#include "transparent_filter_output.h"

#include <spa/param/audio/raw.h>

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

namespace pipetune {

static TransparentFilterOutputEvaluation rejectOutput(
    TransparentFilterOutputRejection rejection, std::string error) {
  return {.output = std::nullopt,
          .rejection = rejection,
          .error = std::move(error)};
}

static bool supportedDeviceApi(const TransparentFilterOutputCandidate &candidate) {
  return candidate.deviceApi == "alsa" || candidate.deviceApi == "bluez5";
}

static bool supportedChannelPosition(std::uint32_t position) noexcept {
  return position >= SPA_AUDIO_CHANNEL_MONO &&
         position < SPA_AUDIO_CHANNEL_START_Custom;
}

static bool supportedChannelLayout(
    const TransparentFilterOutputCandidate &candidate) {
  if (candidate.channelCount == 0 || candidate.channelCount > 8 ||
      candidate.channelPositions.size() != candidate.channelCount) {
    return false;
  }
  auto positions = std::unordered_set<std::uint32_t>{};
  for (const auto position : candidate.channelPositions) {
    if (!supportedChannelPosition(position) ||
        !positions.insert(position).second) {
      return false;
    }
  }
  return true;
}

TransparentFilterOutputEvaluation evaluateTransparentFilterOutput(
    const TransparentFilterOutputCandidate &candidate,
    const SampleRatePolicy &ratePolicy) {
  if (candidate.mediaClass != "Audio/Sink") {
    return rejectOutput(TransparentFilterOutputRejection::notAudioSink,
                        "node is not an Audio/Sink");
  }
  if (candidate.id == 0 || candidate.nodeName.empty()) {
    return rejectOutput(TransparentFilterOutputRejection::missingIdentity,
                        "audio sink has no stable PipeWire identity");
  }
  if (candidate.virtualNode) {
    return rejectOutput(TransparentFilterOutputRejection::virtualNode,
                        "virtual audio sinks are routed directly");
  }
  if (candidate.networkNode) {
    return rejectOutput(TransparentFilterOutputRejection::networkNode,
                        "network audio sinks are routed directly");
  }
  if (candidate.encodedOnly) {
    return rejectOutput(TransparentFilterOutputRejection::encodedOnly,
                        "encoded-only audio sinks are routed directly");
  }
  if (candidate.smartFilter) {
    return rejectOutput(TransparentFilterOutputRejection::smartFilter,
                        "smart-filter nodes are not physical outputs");
  }
  if (!candidate.hasDevice) {
    return rejectOutput(TransparentFilterOutputRejection::notDeviceBacked,
                        "audio sink is not backed by a PipeWire device");
  }
  if (!supportedDeviceApi(candidate)) {
    return rejectOutput(
        TransparentFilterOutputRejection::unsupportedDeviceApi,
        "audio sink is not backed by a supported ALSA or Bluetooth device");
  }
  if (!supportedChannelLayout(candidate)) {
    return rejectOutput(
        TransparentFilterOutputRejection::unsupportedLayout,
        "audio sink layout must contain one through eight exact channels");
  }
  const auto rates = resolveSampleRates(
      ratePolicy, candidate.sampleRateCapabilities,
      candidate.activeSampleRate, candidate.activeSampleRate);
  if (!rates.has_value()) {
    return rejectOutput(TransparentFilterOutputRejection::invalidRatePolicy,
                        "sample-rate policy is invalid");
  }

  const auto filterNodeName =
      "pipetune.filter." + std::to_string(candidate.id);
  return {
      .output = TransparentFilterOutput{
          .id = candidate.id,
          .nodeName = candidate.nodeName,
          .description = candidate.description,
          .filterNodeName = filterNodeName,
          .filterLinkGroup = filterNodeName + ".link-group",
          .channelCount = candidate.channelCount,
          .channelPositions = candidate.channelPositions,
          .sampleRateCapabilities = candidate.sampleRateCapabilities,
          .rates = *rates,
          .activeSampleRate = candidate.activeSampleRate},
      .rejection = TransparentFilterOutputRejection::none,
      .error = {}};
}

TransparentFilterOutputTracker::TransparentFilterOutputTracker(
    SampleRatePolicy ratePolicy)
    : ratePolicy_(sampleRatePolicyIsValid(ratePolicy)
                      ? ratePolicy
                      : defaultSampleRatePolicy()),
      candidates_(), outputs_(), rejectedOutputs_() {}

bool TransparentFilterOutputTracker::update(
    TransparentFilterOutputCandidate candidate) {
  const auto found = candidates_.find(candidate.id);
  if (found != candidates_.end() && found->second == candidate) {
    return false;
  }
  candidates_.insert_or_assign(candidate.id, std::move(candidate));
  rebuildOutputs();
  return true;
}

bool TransparentFilterOutputTracker::remove(std::uint32_t id) {
  if (candidates_.erase(id) == 0) {
    return false;
  }
  rebuildOutputs();
  return true;
}

bool TransparentFilterOutputTracker::setRatePolicy(
    const SampleRatePolicy &ratePolicy) {
  if (!sampleRatePolicyIsValid(ratePolicy) || ratePolicy_ == ratePolicy) {
    return false;
  }
  ratePolicy_ = ratePolicy;
  rebuildOutputs();
  return true;
}

const std::vector<TransparentFilterOutput> &
TransparentFilterOutputTracker::outputs() const noexcept {
  return outputs_;
}

const std::vector<TransparentFilterRejectedOutput> &
TransparentFilterOutputTracker::rejectedOutputs() const noexcept {
  return rejectedOutputs_;
}

void TransparentFilterOutputTracker::rebuildOutputs() {
  auto next = std::vector<TransparentFilterOutput>{};
  auto rejected = std::vector<TransparentFilterRejectedOutput>{};
  next.reserve(candidates_.size());
  rejected.reserve(candidates_.size());
  for (const auto &[id, candidate] : candidates_) {
    static_cast<void>(id);
    auto evaluated = evaluateTransparentFilterOutput(candidate, ratePolicy_);
    if (evaluated.output.has_value()) {
      next.push_back(std::move(*evaluated.output));
    } else {
      rejected.push_back({.id = candidate.id,
                          .nodeName = candidate.nodeName,
                          .description = candidate.description,
                          .sampleRateCapabilities =
                              candidate.sampleRateCapabilities,
                          .activeSampleRate = candidate.activeSampleRate,
                          .rejection = evaluated.rejection,
                          .error = std::move(evaluated.error)});
    }
  }
  std::sort(next.begin(), next.end(),
            [](const TransparentFilterOutput &left,
               const TransparentFilterOutput &right) {
              return left.id < right.id;
            });
  std::sort(rejected.begin(), rejected.end(),
            [](const TransparentFilterRejectedOutput &left,
               const TransparentFilterRejectedOutput &right) {
              return left.id < right.id;
            });
  outputs_ = std::move(next);
  rejectedOutputs_ = std::move(rejected);
}

} // namespace pipetune
