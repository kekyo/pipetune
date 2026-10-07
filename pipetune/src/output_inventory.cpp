/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/output_inventory.h"
#include "pipewire_output_inventory.h"

#include <pipewire/pipewire.h>
#include <yyjson.h>

#include <cstdlib>
#include <memory>
#include <sstream>

namespace pipetune {

struct InventoryQuery {
  pw_main_loop *loop;
  OutputInventoryResult result;
  bool completed = false;
};

static void receivedInventory(const OutputInventoryResult &snapshot, void *data) {
  auto &query = *static_cast<InventoryQuery *>(data);
  if (query.completed) return;
  query.result = snapshot;
  query.completed = true;
  pw_main_loop_quit(query.loop);
}

static void inventoryTimedOut(void *data, std::uint64_t) {
  auto &query = *static_cast<InventoryQuery *>(data);
  receivedInventory({.outputs = {}, .error = "PipeWire output enumeration timed out"}, &query);
}

OutputInventoryResult queryAvailableOutputs() {
  pw_init(nullptr, nullptr);
  auto result = OutputInventoryResult{.outputs = {}, .error = "cannot connect to PipeWire to enumerate outputs"};
  // Each owner is released before its parent. No server objects or routes are
  // created: the connection owns only read-only registry bindings.
  {
    auto loop = std::unique_ptr<pw_main_loop, decltype(&pw_main_loop_destroy)>(
        pw_main_loop_new(nullptr), pw_main_loop_destroy);
    if (loop != nullptr) {
      auto context = std::unique_ptr<pw_context, decltype(&pw_context_destroy)>(
          pw_context_new(pw_main_loop_get_loop(loop.get()), nullptr, 0), pw_context_destroy);
      if (context != nullptr) {
        auto core = std::unique_ptr<pw_core, decltype(&pw_core_disconnect)>(
            pw_context_connect(context.get(), pw_properties_new(
                PW_KEY_APP_NAME, "PipeTune", PW_KEY_APP_ID, "net.kekyo.pipetune", nullptr), 0),
            pw_core_disconnect);
        if (core != nullptr) {
          auto query = InventoryQuery{.loop = loop.get(), .result = result};
          auto observer = observePipeWireOutputs(core.get(), receivedInventory, &query);
          auto *timer = pw_loop_add_timer(pw_main_loop_get_loop(loop.get()), inventoryTimedOut, &query);
          if (observer != nullptr && timer != nullptr) {
            auto deadline = timespec{.tv_sec = 10, .tv_nsec = 0};
            auto interval = timespec{};
            if (pw_loop_update_timer(pw_main_loop_get_loop(loop.get()), timer, &deadline, &interval, false) >= 0) {
              pw_main_loop_run(loop.get());
              result = std::move(query.result);
            }
          }
          if (timer != nullptr) pw_loop_destroy_source(pw_main_loop_get_loop(loop.get()), timer);
        }
      }
    }
  }
  pw_deinit();
  return result;
}

static std::string selectionError(const AvailableOutput &output) {
  return appendConfiguredOutput({}, {.id = "candidate", .enabled = true, .device = output.device}).error;
}

std::string formatOutputInventory(std::span<const AvailableOutput> outputs, bool json) {
  if (!json) {
    if (outputs.empty()) return "No audio outputs are available.";
    auto text = std::ostringstream{};
    for (const auto &output : outputs) {
      text << output.device.name << " [" << output.nodeName << "]\n"
           << "  Profile: " << (output.device.profile.empty() ? "none" : output.device.profile) << "\n"
           << "  Channels: ";
      for (auto index = std::size_t{0}; index < output.device.channelPositions.size(); ++index) {
        if (index != 0) text << ", ";
        text << index + 1 << ": " << output.device.channelPositions[index];
      }
      if (output.device.channelPositions.empty()) text << "unavailable";
      text << '\n';
      const auto error = selectionError(output);
      if (!error.empty()) text << "  Cannot select: " << error << '\n';
    }
    return text.str();
  }
  auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  if (document == nullptr) return {};
  auto *root = yyjson_mut_obj(document.get());
  if (root == nullptr) return {};
  yyjson_mut_doc_set_root(document.get(), root);
  auto *array = yyjson_mut_obj_add_arr(document.get(), root, "outputs");
  if (array == nullptr) return {};
  for (const auto &output : outputs) {
    auto *object = yyjson_mut_arr_add_obj(document.get(), array);
    auto *identity = yyjson_mut_obj_add_obj(document.get(), object, "identity");
    auto *channels = yyjson_mut_obj_add_arr(document.get(), object, "channelPositions");
    if (object == nullptr || identity == nullptr || channels == nullptr) return {};
    const auto add = [&](yyjson_mut_val *target, const char *key, const std::string &value) {
      return yyjson_mut_obj_add_strcpy(document.get(), target, key, value.c_str());
    };
    const auto error = selectionError(output);
    if (!yyjson_mut_obj_add_uint(document.get(), object, "nodeId", output.nodeId) ||
        !add(object, "nodeSerial", std::to_string(output.nodeSerial)) ||
        !yyjson_mut_obj_add_bool(document.get(), object, "selectable", error.empty()) ||
        !add(object, "nodeName", output.nodeName) || !add(object, "name", output.device.name) ||
        !add(object, "profile", output.device.profile) || !add(object, "error", error) ||
        !add(identity, "api", output.device.identity.api) || !add(identity, "location", output.device.identity.location) ||
        !add(identity, "port", output.device.identity.port) || !add(identity, "vendor", output.device.identity.vendor) ||
        !add(identity, "product", output.device.identity.product) || !add(identity, "serial", output.device.identity.serial)) return {};
    for (const auto &position : output.device.channelPositions) {
      if (!yyjson_mut_arr_add_strcpy(document.get(), channels, position.c_str())) return {};
    }
  }
  auto length = std::size_t{0};
  auto *encoded = yyjson_mut_write(document.get(), YYJSON_WRITE_ESCAPE_UNICODE | YYJSON_WRITE_ALLOW_INVALID_UNICODE, &length);
  if (encoded == nullptr) return {};
  auto result = std::string(encoded, length);
  std::free(encoded);
  return result;
}

} // namespace pipetune
