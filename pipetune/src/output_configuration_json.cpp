/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/output_configuration.h"

#include <yyjson.h>

#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <memory>
#include <utility>

namespace pipetune {

static bool hasFields(yyjson_val *value, std::initializer_list<const char *> names) {
  if (!yyjson_is_obj(value) || yyjson_obj_size(value) != names.size()) return false;
  // Every required name must be present exactly once. The exact size also
  // rejects duplicates and unknown fields without silently losing settings.
  for (const auto *name : names) {
    if (yyjson_obj_get(value, name) == nullptr) return false;
  }
  return true;
}

static bool readString(yyjson_val *value, std::string &destination) {
  if (!yyjson_is_str(value)) return false;
  destination.assign(yyjson_get_str(value), yyjson_get_len(value));
  return true;
}

OutputConfigurationResult parseOutputConfiguration(std::string_view json) {
  const auto fail = [](std::string error) {
    return OutputConfigurationResult{.configuration = {}, .error = std::move(error)};
  };
  if (json.size() > 64 * 1024) return fail("output configuration exceeds 64 KiB");
  auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG), yyjson_doc_free);
  if (document == nullptr) return fail("output configuration is not valid JSON");
  auto *root = yyjson_doc_get_root(document.get());
  if (!hasFields(root, {"mode", "outputs", "channels"}))
    return fail("output configuration requires exactly mode, outputs, and channels");
  auto configuration = OutputConfiguration{};
  auto mode = std::string{};
  if (!readString(yyjson_obj_get(root, "mode"), mode) || (mode != "single" && mode != "multiple"))
    return fail("output mode must be single or multiple");
  configuration.mode = mode == "single" ? OutputMode::single : OutputMode::multiple;
  auto *outputs = yyjson_obj_get(root, "outputs");
  auto *channels = yyjson_obj_get(root, "channels");
  if (!yyjson_is_arr(outputs) || !yyjson_is_arr(channels) ||
      yyjson_arr_size(outputs) > 16 || yyjson_arr_size(channels) > 16)
    return fail("output and channel arrays must contain at most sixteen entries");
  for (auto index = std::size_t{0}; index < yyjson_arr_size(outputs); ++index) {
    auto *value = yyjson_arr_get(outputs, index);
    if (!hasFields(value, {"id", "enabled", "device"}))
      return fail("configured output requires exactly id, enabled, and device");
    auto output = ConfiguredOutput{};
    auto *enabled = yyjson_obj_get(value, "enabled");
    if (!readString(yyjson_obj_get(value, "id"), output.id) || !yyjson_is_bool(enabled))
      return fail("output id must be a string and enabled must be a boolean");
    output.enabled = yyjson_get_bool(enabled);
    auto *device = yyjson_obj_get(value, "device");
    if (!hasFields(device, {"identity", "name", "profile", "channelPositions"}))
      return fail("output device requires exactly identity, name, profile, and channelPositions");
    auto *identity = yyjson_obj_get(device, "identity");
    auto &saved = output.device;
    if (!hasFields(identity, {"api", "location", "port", "vendor", "product", "serial"}) ||
        !readString(yyjson_obj_get(identity, "api"), saved.identity.api) ||
        !readString(yyjson_obj_get(identity, "location"), saved.identity.location) ||
        !readString(yyjson_obj_get(identity, "port"), saved.identity.port) ||
        !readString(yyjson_obj_get(identity, "vendor"), saved.identity.vendor) ||
        !readString(yyjson_obj_get(identity, "product"), saved.identity.product) ||
        !readString(yyjson_obj_get(identity, "serial"), saved.identity.serial) ||
        !readString(yyjson_obj_get(device, "name"), saved.name) ||
        !readString(yyjson_obj_get(device, "profile"), saved.profile))
      return fail("output identity, name, and profile must have all required string fields");
    auto *positions = yyjson_obj_get(device, "channelPositions");
    if (!yyjson_is_arr(positions) || yyjson_arr_size(positions) > 16)
      return fail("output channelPositions must be an array of at most sixteen strings");
    for (auto channel = std::size_t{0}; channel < yyjson_arr_size(positions); ++channel) {
      auto position = std::string{};
      if (!readString(yyjson_arr_get(positions, channel), position))
        return fail("output channel positions must be strings");
      saved.channelPositions.push_back(std::move(position));
    }
    configuration.outputs.push_back(std::move(output));
  }
  for (auto index = std::size_t{0}; index < yyjson_arr_size(channels); ++index) {
    auto *value = yyjson_arr_get(channels, index);
    if (!hasFields(value, {"outputId", "deviceChannel", "label"}))
      return fail("output channel requires exactly outputId, deviceChannel, and label");
    auto slot = OutputChannelSlot{};
    auto *channel = yyjson_obj_get(value, "deviceChannel");
    if (!readString(yyjson_obj_get(value, "outputId"), slot.outputId) ||
        !readString(yyjson_obj_get(value, "label"), slot.label) ||
        !yyjson_is_uint(channel) || yyjson_get_uint(channel) > std::numeric_limits<std::uint32_t>::max())
      return fail("channel identifiers and labels must be strings, and deviceChannel an unsigned integer");
    slot.deviceChannel = static_cast<std::uint32_t>(yyjson_get_uint(channel));
    configuration.channels.push_back(std::move(slot));
  }
  const auto error = validateOutputConfiguration(configuration);
  if (!error.empty()) return fail(error);
  return {.configuration = std::move(configuration), .error = {}};
}

std::string formatOutputConfiguration(const OutputConfiguration &configuration) {
  if (!validateOutputConfiguration(configuration).empty()) return {};
  auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  if (document == nullptr) return {};
  auto *doc = document.get();
  auto *root = yyjson_mut_obj(doc);
  if (root == nullptr) return {};
  yyjson_mut_doc_set_root(doc, root);
  const auto add = [doc](yyjson_mut_val *target, const char *key, const std::string &value) {
    return yyjson_mut_obj_add_strncpy(doc, target, key, value.data(), value.size());
  };
  if (!yyjson_mut_obj_add_str(doc, root, "mode", configuration.mode == OutputMode::single ? "single" : "multiple")) return {};
  auto *outputs = yyjson_mut_obj_add_arr(doc, root, "outputs");
  auto *channels = yyjson_mut_obj_add_arr(doc, root, "channels");
  if (outputs == nullptr || channels == nullptr) return {};
  for (const auto &output : configuration.outputs) {
    auto *value = yyjson_mut_arr_add_obj(doc, outputs);
    auto *device = yyjson_mut_obj_add_obj(doc, value, "device");
    auto *identity = yyjson_mut_obj_add_obj(doc, device, "identity");
    auto *positions = yyjson_mut_obj_add_arr(doc, device, "channelPositions");
    if (value == nullptr || device == nullptr || identity == nullptr || positions == nullptr) return {};
    const auto &saved = output.device;
    if (!add(value, "id", output.id) || !yyjson_mut_obj_add_bool(doc, value, "enabled", output.enabled) ||
        !add(device, "name", saved.name) || !add(device, "profile", saved.profile) ||
        !add(identity, "api", saved.identity.api) || !add(identity, "location", saved.identity.location) ||
        !add(identity, "port", saved.identity.port) || !add(identity, "vendor", saved.identity.vendor) ||
        !add(identity, "product", saved.identity.product) || !add(identity, "serial", saved.identity.serial)) return {};
    for (const auto &position : saved.channelPositions) {
      if (!yyjson_mut_arr_add_strncpy(doc, positions, position.data(), position.size())) return {};
    }
  }
  for (const auto &slot : configuration.channels) {
    auto *value = yyjson_mut_arr_add_obj(doc, channels);
    if (value == nullptr || !add(value, "outputId", slot.outputId) || !add(value, "label", slot.label) ||
        !yyjson_mut_obj_add_uint(doc, value, "deviceChannel", slot.deviceChannel)) return {};
  }
  auto length = std::size_t{0};
  auto encoded = std::unique_ptr<char, decltype(&std::free)>(
      yyjson_mut_write(doc, YYJSON_WRITE_NOFLAG, &length), std::free);
  if (encoded == nullptr || length > 64 * 1024) return {};
  return std::string(encoded.get(), length);
}

} // namespace pipetune
