/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output_distribution.h"

#include <yyjson.h>

#include <algorithm>
#include <cstdlib>
#include <memory>

namespace pipetune {

OutputDistributionArguments makeOutputDistributionArguments(
    const std::string &nodeName, const std::string &publicInputName, const OutputConfiguration &configuration,
    std::span<const AvailableOutput> inventory) {
  const auto error = validateOutputConfiguration(configuration);
  if (!error.empty()) return {{}, error};
  if (nodeName.empty() || nodeName.find('\0') != std::string::npos || publicInputName.empty() ||
      publicInputName.find('\0') != std::string::npos || configuration.mode != OutputMode::multiple)
    return {{}, "output distribution requires a node name and multiple mode"};
  const auto resolved = resolveConfiguredOutputs(configuration, inventory);
  auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  const auto allocationError = OutputDistributionArguments{{}, "cannot encode output distribution"};
  if (document == nullptr) return allocationError;
  auto *root = yyjson_mut_obj(document.get());
  if (root == nullptr) return allocationError;
  yyjson_mut_doc_set_root(document.get(), root);
  const auto add = [&](yyjson_mut_val *object, const char *key, const std::string &value) {
    return yyjson_mut_obj_add_strcpy(document.get(), object, key, value.c_str());
  };
  auto *combined = yyjson_mut_obj_add_obj(document.get(), root, "combine.props");
  auto *stream = yyjson_mut_obj_add_obj(document.get(), root, "stream.props");
  auto *positions = yyjson_mut_obj_add_arr(document.get(), combined, "audio.position");
  auto *rules = yyjson_mut_obj_add_arr(document.get(), root, "stream.rules");
  if (combined == nullptr || stream == nullptr || positions == nullptr || rules == nullptr ||
      !add(root, "combine.mode", "sink") || !add(root, "node.name", nodeName) ||
      !add(root, "node.description", "PipeTune output distribution") ||
      !yyjson_mut_obj_add_bool(document.get(), root, "combine.latency-compensate", true) ||
      !yyjson_mut_obj_add_bool(document.get(), combined, "node.virtual", true) ||
      !yyjson_mut_obj_add_bool(document.get(), combined, "node.pipetune.internal", true) ||
      !yyjson_mut_obj_add_bool(document.get(), combined, "node.pipetune.aggregate", true) ||
      !yyjson_mut_obj_add_bool(document.get(), combined, "node.pipetune.manage-default", true) ||
      !add(combined, "node.pipetune.public-target", publicInputName) ||
      !yyjson_mut_obj_add_bool(document.get(), stream, "stream.dont-remix", true) ||
      !yyjson_mut_obj_add_bool(document.get(), stream, "node.dont-fallback", true) ||
      !yyjson_mut_obj_add_bool(document.get(), stream, "node.dont-move", true) ||
      !yyjson_mut_obj_add_bool(document.get(), stream, "node.pipetune.managed-output", true) ||
      !add(stream, "node.pipetune.public-target", publicInputName) ||
      !add(stream, "media.role", "PipeTune-Filter-Output")) return allocationError;
  for (auto slot = 0U; slot < outputDspChannelCount(configuration); ++slot) {
    const auto position = "AUX" + std::to_string(slot);
    if (!yyjson_mut_arr_add_strcpy(document.get(), positions, position.c_str())) return allocationError;
  }
  for (auto index = std::size_t{0}; index < resolved.size(); ++index) {
    if (!resolved[index].inventoryIndex) continue;
    const auto &available = inventory[*resolved[index].inventoryIndex];
    const auto &output = configuration.outputs[index];
    if (available.nodeSerial == 0) return {{}, "resolved output has no PipeWire object serial"};
    auto *rule = yyjson_mut_arr_add_obj(document.get(), rules);
    auto *matches = yyjson_mut_obj_add_arr(document.get(), rule, "matches");
    auto *match = yyjson_mut_arr_add_obj(document.get(), matches);
    auto *actions = yyjson_mut_obj_add_obj(document.get(), rule, "actions");
    auto *create = yyjson_mut_obj_add_obj(document.get(), actions, "create-stream");
    auto *source = yyjson_mut_obj_add_arr(document.get(), create, "combine.audio.position");
    auto *target = yyjson_mut_obj_add_arr(document.get(), create, "audio.position");
    if (rule == nullptr || matches == nullptr || match == nullptr || actions == nullptr || create == nullptr ||
        source == nullptr || target == nullptr ||
        !add(match, "object.serial", std::to_string(available.nodeSerial)) ||
        !add(create, "node.pipetune.output-id", output.id) ||
        !add(create, "node.pipetune.target-serial", std::to_string(available.nodeSerial)) ||
        !add(create, "node.pipetune.output-channels", std::to_string(output.device.channelPositions.size()))) return allocationError;
    // Match this exact connection generation. Reused node IDs or names cannot
    // redirect a saved mapping before a fresh identity resolution is complete.
    auto channelSlots = std::string{};
    for (auto channel = std::size_t{0}; channel < output.device.channelPositions.size(); ++channel) {
      const auto slot = std::find_if(configuration.channels.begin(), configuration.channels.end(),
          [&](const auto &candidate) { return candidate.outputId == output.id && candidate.deviceChannel == channel; });
      const auto position = "AUX" + std::to_string(slot - configuration.channels.begin());
      if (!channelSlots.empty()) channelSlots += ',';
      channelSlots += std::to_string(slot - configuration.channels.begin());
      if (!yyjson_mut_arr_add_strcpy(document.get(), source, position.c_str()) ||
          !yyjson_mut_arr_add_strcpy(document.get(), target, output.device.channelPositions[channel].c_str())) return allocationError;
    }
    if (!add(create, "node.pipetune.output-slots", channelSlots)) return allocationError;
  }
  auto length = std::size_t{0};
  auto *encoded = yyjson_mut_write(document.get(), YYJSON_WRITE_ESCAPE_UNICODE, &length);
  if (encoded == nullptr) return allocationError;
  auto arguments = std::string(encoded, length);
  std::free(encoded);
  return {std::move(arguments), {}};
}

// Match the public layouts used by desktop applications; the loopback maps
// these channels by index to the private AUX bus without surround remixing.
static std::vector<std::string> directInputPositions(std::uint32_t channels) {
  switch (channels) {
  case 2: return {"FL", "FR"};
  case 3: return {"FL", "FR", "FC"};
  case 4: return {"FL", "FR", "RL", "RR"};
  case 5: return {"FL", "FR", "FC", "RL", "RR"};
  case 6: return {"FL", "FR", "FC", "LFE", "RL", "RR"};
  case 7: return {"FL", "FR", "FC", "LFE", "RC", "SL", "SR"};
  case 8: return {"FL", "FR", "FC", "LFE", "RL", "RR", "SL", "SR"};
  default: {
    auto positions = std::vector<std::string>{};
    for (auto channel = 0U; channel < channels; ++channel) positions.push_back("AUX" + std::to_string(channel));
    return positions;
  }
  }
}

OutputDistributionArguments makeDirectOutputArguments(
    const std::string &publicInputName, std::uint32_t channels,
    const SampleRatePolicy &policy) {
  if (publicInputName.empty() || publicInputName.find('\0') != std::string::npos ||
      channels < 2 || channels > 16 || !sampleRatePolicyIsValid(policy))
    return {{}, "invalid direct output configuration"};
  auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  const auto error = OutputDistributionArguments{{}, "cannot encode direct output"};
  if (!document) return error;
  auto *root = yyjson_mut_obj(document.get());
  if (!root) return error;
  yyjson_mut_doc_set_root(document.get(), root);
  auto *capture = yyjson_mut_obj_add_obj(document.get(), root, "capture.props");
  auto *playback = yyjson_mut_obj_add_obj(document.get(), root, "playback.props");
  if (!capture || !playback) return error;
  const auto add = [&](yyjson_mut_val *object, const char *key, const std::string &value) {
    return yyjson_mut_obj_add_strcpy(document.get(), object, key, value.c_str());
  };
  if (!add(root, "node.description", "PipeTune Direct Output") ||
      !add(capture, "node.name", publicInputName + ".direct") ||
      !add(capture, "media.class", "Audio/Sink") ||
      !yyjson_mut_obj_add_bool(document.get(), capture, "node.virtual", true) ||
      !yyjson_mut_obj_add_bool(document.get(), capture, "node.pipetune.public-sink", true) ||
      !yyjson_mut_obj_add_bool(document.get(), capture, "node.pipetune.direct", true) ||
      !yyjson_mut_obj_add_bool(document.get(), capture, "channelmix.upmix", false) ||
      !add(playback, "node.name", publicInputName + ".direct.output") ||
      !add(playback, "target.object", publicInputName + ".distribution") ||
      !add(playback, "media.role", "PipeTune-Filter-Output") ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "node.passive", true) ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "node.pipetune.internal", true) ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "node.pipetune.managed-output", true) ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "node.dont-fallback", true) ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "node.dont-move", true) ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "node.linger", true) ||
      !yyjson_mut_obj_add_bool(document.get(), playback, "stream.dont-remix", true)) return error;
  const auto inputPositions = directInputPositions(channels);
  for (auto *properties : {capture, playback}) {
    if (!add(properties, "audio.format", "F32P") ||
        !yyjson_mut_obj_add_uint(document.get(), properties, "audio.channels", channels)) return error;
    auto *positions = yyjson_mut_obj_add_arr(document.get(), properties, "audio.position");
    if (!positions) return error;
    for (auto channel = 0U; channel < channels; ++channel) {
      const auto position = properties == capture ? inputPositions[channel] : "AUX" + std::to_string(channel);
      if (!yyjson_mut_arr_add_strcpy(document.get(), positions, position.c_str())) return error;
    }
    if (policy.mode == SampleRateMode::fixed &&
        (!add(properties, "node.rate", "1/" + std::to_string(policy.fixedRate)) ||
         !yyjson_mut_obj_add_uint(document.get(), properties, "audio.rate", policy.fixedRate))) return error;
  }
  // Direct playback must keep the shared clock request alive when the DSP
  // input is idle. Both branches request the same rate, so they cannot compete.
  if (policy.mode == SampleRateMode::fixed && policy.enforcement == SampleRateEnforcement::force &&
      !add(playback, "node.force-rate", "0")) return error;
  auto length = std::size_t{0};
  auto *encoded = yyjson_mut_write(document.get(), YYJSON_WRITE_ESCAPE_UNICODE, &length);
  if (!encoded) return error;
  auto arguments = std::string(encoded, length);
  std::free(encoded);
  return {std::move(arguments), {}};
}

} // namespace pipetune
