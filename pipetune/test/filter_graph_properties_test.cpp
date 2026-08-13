/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "filter_graph_properties.h"

#include <iostream>
#include <optional>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static std::optional<std::string_view> property(
    const pipetune::FilterNodeProperties &properties,
    std::string_view key) {
  for (const auto &[candidateKey, value] : properties) {
    if (candidateKey == key) {
      return value;
    }
  }
  return std::nullopt;
}

static bool testSmartFilterPairContract() {
  const auto graph = pipetune::makeFilterGraphProperties(
      {.nodeName = "pipetune",
       .nodeDescription = "PipeTune",
       .fixedSampleRate = std::nullopt,
       .channelCount = 2,
       .forceRate = false});

  return check(property(graph.input, "media.class") == "Audio/Sink",
               "filter input must be an Audio/Sink") &&
         check(property(graph.input, "node.pipetune.internal") == "true",
               "filter input must identify itself as an internal node") &&
         check(property(graph.input, "filter.smart") == "true",
               "filter input must opt in to WirePlumber smart filtering") &&
         check(property(graph.input, "filter.smart.name") ==
                   "net.kekyo.pipetune",
               "filter input must publish a stable smart-filter name") &&
         check(property(graph.input, "target.endpoint") ==
                   "endpoint.pipetune.playback",
               "filter input must identify the WirePlumber 0.4 endpoint") &&
         check(property(graph.output, "media.role") ==
                   "PipeTune-Filter-Output",
               "filter output must bypass WirePlumber 0.4 client endpoint "
               "routing") &&
         check(property(graph.input, "node.pipetune.target-endpoint") ==
                   "endpoint.pipetune.playback",
               "filter input must expose the pre-0.4.16 endpoint target") &&
         check(property(graph.output, "media.class") ==
                   "Stream/Output/Audio",
               "filter output must be a playback stream") &&
         check(property(graph.output, "node.passive") == "true",
               "filter output must be passive") &&
         check(property(graph.output, "stream.dont-remix") == "true",
               "filter output must preserve the negotiated channels") &&
         check(property(graph.input, "node.link-group") ==
                   property(graph.output, "node.link-group"),
               "filter nodes must share one link group") &&
         check(!property(graph.output, "target.object").has_value(),
               "filter output must follow WirePlumber policy") &&
         check(!property(graph.input, "audio.rate").has_value() &&
                   !property(graph.output, "audio.rate").has_value() &&
                   !property(graph.input, "node.rate").has_value() &&
                   !property(graph.output, "node.rate").has_value(),
               "automatic filtering must leave the graph rate negotiable") &&
         check(graph.outputAutoconnect,
               "filter output must ask the session manager to connect it") &&
         check(graph.outputReconnect,
               "filter output must reconnect after default-device changes");
}

static bool testFixedGraphRateContract() {
  const auto graph = pipetune::makeFilterGraphProperties(
      {.nodeName = "pipetune",
       .nodeDescription = "PipeTune",
       .fixedSampleRate = 96000,
       .channelCount = 2,
       .forceRate = true});

  return check(property(graph.input, "audio.rate") == "96000" &&
                   property(graph.output, "audio.rate") == "96000" &&
                   property(graph.input, "node.rate") == "1/96000" &&
                   property(graph.output, "node.rate") == "1/96000",
               "fixed filtering must constrain both nodes to one rate") &&
         check(property(graph.output, "node.force-rate") == "0",
               "force mode must request the fixed graph rate");
}

int main() {
  return testSmartFilterPairContract() && testFixedGraphRateContract() ? 0
                                                                       : 1;
}
