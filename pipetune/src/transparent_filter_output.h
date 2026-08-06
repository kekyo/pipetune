#ifndef PIPETUNE_TRANSPARENT_FILTER_OUTPUT_H
#define PIPETUNE_TRANSPARENT_FILTER_OUTPUT_H

#include "pipetune/sample_rate.h"

#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace pipetune {

/** Explains why a PipeWire sink cannot host a PipeTune transparent filter. */
enum class TransparentFilterOutputRejection {
  /** The output is eligible. */
  none,
  /** The registry object is not an audio sink. */
  notAudioSink,
  /** The node has no stable PipeWire identity. */
  missingIdentity,
  /** The node is itself a virtual output. */
  virtualNode,
  /** The node is a network output. */
  networkNode,
  /** The node accepts only encoded or passthrough audio. */
  encodedOnly,
  /** The node is another smart filter rather than an output device. */
  smartFilter,
  /** The sink is not backed by a PipeWire device. */
  notDeviceBacked,
  /** The backing device API is outside the supported local-output set. */
  unsupportedDeviceApi,
  /** The channel count or positions cannot be reproduced exactly. */
  unsupportedLayout,
  /** The configured sample-rate policy is invalid. */
  invalidRatePolicy
};

/**
 * Contains the registry and format facts used to classify one PipeWire node.
 */
struct TransparentFilterOutputCandidate {
  /** PipeWire global ID. */
  std::uint32_t id;
  /** PipeWire media.class. */
  std::string mediaClass;
  /** PipeWire node.name. */
  std::string nodeName;
  /** User-facing node description. */
  std::string description;
  /** Backing device.api, such as alsa or bluez5. */
  std::string deviceApi;
  /** True when the node exposes a valid device.id. */
  bool hasDevice;
  /** Parsed node.virtual value. */
  bool virtualNode;
  /** True for network-backed sinks. */
  bool networkNode;
  /** True for encoded-only or passthrough sinks. */
  bool encodedOnly;
  /** True when the node declares filter.smart. */
  bool smartFilter;
  /** Exact number of channels reported by the sink. */
  std::uint32_t channelCount;
  /** Exact SPA audio channel positions in sink order. */
  std::vector<std::uint32_t> channelPositions;
  /** Enumerated sink sample-rate capabilities. */
  SampleRateCapabilities sampleRateCapabilities;
  /** Active sink rate, or zero before Format is available. */
  std::uint32_t activeSampleRate;

  /** Compares all classification and format facts. */
  bool operator==(const TransparentFilterOutputCandidate &) const = default;
};

/** Describes one physical output and the filter runtime it requires. */
struct TransparentFilterOutput {
  /** PipeWire global ID of the physical sink. */
  std::uint32_t id;
  /** Physical sink node.name used as the smart-filter target. */
  std::string nodeName;
  /** User-facing physical sink description. */
  std::string description;
  /** Internal PipeTune smart-filter main-node name. */
  std::string filterNodeName;
  /** Shared node.link-group for the filter's two streams. */
  std::string filterLinkGroup;
  /** Exact number of channels processed for this output. */
  std::uint32_t channelCount;
  /** Exact SPA channel positions copied from the physical sink. */
  std::vector<std::uint32_t> channelPositions;
  /** Enumerated sample-rate capabilities copied from the physical sink. */
  SampleRateCapabilities sampleRateCapabilities;
  /** DSP and playback rates resolved only for this physical output. */
  ResolvedSampleRates rates;
  /** Active physical rate observed from PipeWire, or zero. */
  std::uint32_t activeSampleRate;

  /** Compares complete runtime requirements. */
  bool operator==(const TransparentFilterOutput &) const = default;
};

/** Reports either one usable output or its direct-routing reason. */
struct TransparentFilterOutputEvaluation {
  /** Usable per-output filter description. */
  std::optional<TransparentFilterOutput> output;
  /** Machine-readable eligibility result. */
  TransparentFilterOutputRejection rejection;
  /** Human-readable rejection diagnostic, empty for an eligible output. */
  std::string error;
};

/** Describes one sink that must remain on WirePlumber's direct route. */
struct TransparentFilterRejectedOutput {
  /** PipeWire global ID of the rejected sink. */
  std::uint32_t id;
  /** Sink node.name, or empty when the sink has no stable identity. */
  std::string nodeName;
  /** User-facing sink description. */
  std::string description;
  /** Enumerated sink sample-rate capabilities, when available. */
  SampleRateCapabilities sampleRateCapabilities;
  /** Active sink rate observed from PipeWire, or zero. */
  std::uint32_t activeSampleRate;
  /** Machine-readable direct-routing reason. */
  TransparentFilterOutputRejection rejection;
  /** Human-readable direct-routing diagnostic. */
  std::string error;

  /** Compares the complete rejection status. */
  bool operator==(const TransparentFilterRejectedOutput &) const = default;
};

/**
 * Classifies and resolves one PipeWire output without mutating graph state.
 *
 * @param candidate Registry and format facts for one node.
 * @param ratePolicy Current global rate policy, resolved per output.
 * @return Eligible runtime description or a direct-routing diagnostic.
 */
TransparentFilterOutputEvaluation evaluateTransparentFilterOutput(
    const TransparentFilterOutputCandidate &candidate,
    const SampleRatePolicy &ratePolicy);

/** Maintains the independently resolved set of physical output filters. */
class TransparentFilterOutputTracker final {
public:
  /**
   * Creates an empty tracker.
   *
   * @param ratePolicy Initial valid sample-rate policy.
   */
  explicit TransparentFilterOutputTracker(SampleRatePolicy ratePolicy);

  /**
   * Inserts or refreshes one PipeWire node.
   *
   * @param candidate Latest node facts.
   * @return True when any tracked fact or eligible runtime changed.
   */
  bool update(TransparentFilterOutputCandidate candidate);

  /**
   * Removes one PipeWire node.
   *
   * @param id Removed PipeWire global ID.
   * @return True when the node was known.
   */
  bool remove(std::uint32_t id);

  /**
   * Re-resolves every output for a new global policy.
   *
   * @param ratePolicy Candidate valid rate policy.
   * @return True when a different valid policy was accepted.
   */
  bool setRatePolicy(const SampleRatePolicy &ratePolicy);

  /**
   * Returns eligible outputs sorted by PipeWire global ID.
   *
   * @return Stable view valid until the next mutation.
   */
  const std::vector<TransparentFilterOutput> &outputs() const noexcept;

  /**
   * Returns rejected sinks sorted by PipeWire global ID.
   *
   * @return Stable view valid until the next mutation.
   */
  const std::vector<TransparentFilterRejectedOutput> &
  rejectedOutputs() const noexcept;

private:
  void rebuildOutputs();

  SampleRatePolicy ratePolicy_;
  std::unordered_map<std::uint32_t, TransparentFilterOutputCandidate>
      candidates_;
  std::vector<TransparentFilterOutput> outputs_;
  std::vector<TransparentFilterRejectedOutput> rejectedOutputs_;
};

} // namespace pipetune

#endif
