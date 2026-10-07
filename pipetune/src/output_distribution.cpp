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

} // namespace pipetune
