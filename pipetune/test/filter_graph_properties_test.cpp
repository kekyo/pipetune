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
       .sampleRate = 48000,
       .outputNodeRate = 48000,
       .channelCount = 2,
       .forceRate = false});

  return check(property(graph.input, "media.class") == "Audio/Sink",
               "filter input must be an Audio/Sink") &&
         check(property(graph.input, "filter.smart") == "true",
               "filter input must opt in to WirePlumber smart filtering") &&
         check(property(graph.input, "filter.smart.name") ==
                   "net.kekyo.pipetune",
               "filter input must publish a stable smart-filter name") &&
         check(property(graph.input, "target.endpoint") ==
                   "pipetune.playback",
               "filter input must identify the WirePlumber 0.4 endpoint") &&
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
         check(graph.outputAutoconnect,
               "filter output must ask the session manager to connect it") &&
         check(graph.outputReconnect,
               "filter output must reconnect after default-device changes");
}

int main() {
  return testSmartFilterPairContract() ? 0 : 1;
}
