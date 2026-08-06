#include "filter_graph_properties.h"

#include <string>
#include <string_view>

namespace pipetune {

static void appendCommonProperties(FilterNodeProperties &properties,
                                   const FilterGraphPropertyOptions &options,
                                   std::string_view nodeName,
                                   std::uint32_t nodeRate) {
  const auto group = options.nodeName + ".group";
  const auto linkGroup = options.nodeName + ".link-group";
  properties.emplace_back("application.name", "PipeTune");
  properties.emplace_back("media.type", "Audio");
  properties.emplace_back("media.role", "DSP");
  properties.emplace_back("node.name", nodeName);
  properties.emplace_back("node.group", group);
  properties.emplace_back("node.link-group", linkGroup);
  properties.emplace_back("node.rate", "1/" + std::to_string(nodeRate));
  properties.emplace_back("audio.format", "F32P");
  properties.emplace_back("audio.rate", std::to_string(options.sampleRate));
  properties.emplace_back("audio.channels",
                          std::to_string(options.channelCount));
  properties.emplace_back("state.restore-props", "false");
}

FilterGraphProperties makeFilterGraphProperties(
    const FilterGraphPropertyOptions &options) {
  auto result = FilterGraphProperties{
      .input = {},
      .output = {},
      .inputAutoconnect = false,
      .inputReconnect = true,
      .outputAutoconnect = true,
      .outputReconnect = true};

  appendCommonProperties(result.input, options, options.nodeName,
                         options.sampleRate);
  result.input.emplace_back("media.class", "Audio/Sink");
  result.input.emplace_back("media.category", "Playback");
  result.input.emplace_back("node.description", options.nodeDescription);
  result.input.emplace_back("node.virtual", "true");
  result.input.emplace_back("node.always-process", "true");
  result.input.emplace_back("filter.smart", "true");
  result.input.emplace_back("filter.smart.name", "net.kekyo.pipetune");
  result.input.emplace_back("target.endpoint", "endpoint.pipetune.playback");
  result.input.emplace_back("channelmix.min-volume", "1.0");
  result.input.emplace_back("channelmix.max-volume", "1.0");

  appendCommonProperties(result.output, options,
                         options.nodeName + ".output",
                         options.outputNodeRate);
  result.output.emplace_back("media.class", "Stream/Output/Audio");
  result.output.emplace_back("media.category", "Playback");
  result.output.emplace_back("node.passive", "true");
  result.output.emplace_back("stream.dont-remix", "true");
  result.output.emplace_back("channelmix.min-volume", "1.0");
  result.output.emplace_back("channelmix.max-volume", "1.0");
  if (options.forceRate) {
    result.output.emplace_back("node.force-rate", "0");
  }
  return result;
}

} // namespace pipetune
