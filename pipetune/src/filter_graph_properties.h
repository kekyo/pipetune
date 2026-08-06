#ifndef PIPETUNE_FILTER_GRAPH_PROPERTIES_H
#define PIPETUNE_FILTER_GRAPH_PROPERTIES_H

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace pipetune {

/** One ordered set of PipeWire node properties. */
using FilterNodeProperties =
    std::vector<std::pair<std::string, std::string>>;

/** Values shared by the two nodes that form the PipeTune filter. */
struct FilterGraphPropertyOptions {
  /** Stable name of the filter input node. */
  std::string nodeName;
  /** Human-readable description of the filter input node. */
  std::string nodeDescription;
  /** Fixed PCM rate, or no value when PipeWire negotiates the graph rate. */
  std::optional<std::uint32_t> fixedSampleRate;
  /** Number of planar audio channels. */
  std::uint32_t channelCount;
  /** True when the output node must force a graph-rate change. */
  bool forceRate;
};

/** Properties and connection behavior for one filter node pair. */
struct FilterGraphProperties {
  /** Properties published by the filter input node. */
  FilterNodeProperties input;
  /** Properties published by the filter output node. */
  FilterNodeProperties output;
  /** True when the input asks PipeWire to autoconnect it. */
  bool inputAutoconnect;
  /** True when the input may reconnect after a graph change. */
  bool inputReconnect;
  /** True when the output asks PipeWire to autoconnect it. */
  bool outputAutoconnect;
  /** True when the output may reconnect after a graph change. */
  bool outputReconnect;
};

/**
 * Builds the WirePlumber 0.4 endpoint and 0.5 smart-filter node contract.
 *
 * @param options Stable identity and negotiated audio format.
 * @return Properties for the input and output PipeWire nodes.
 */
FilterGraphProperties makeFilterGraphProperties(
    const FilterGraphPropertyOptions &options);

} // namespace pipetune

#endif
