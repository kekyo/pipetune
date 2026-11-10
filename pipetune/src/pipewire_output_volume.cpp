/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_volume.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <charconv>
#include <cstring>
#include <map>

namespace pipetune {

struct VolumeBinding {
  pw_node *node = nullptr;
  spa_hook listener = {};
  std::uint64_t serial = 0;
  ~VolumeBinding() {
    if (node) {
      spa_hook_remove(&listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(node));
    }
  }
};

struct PipeWireOutputVolume {
  pw_registry *registry = nullptr;
  spa_hook listener = {};
  std::string name;
  PipeWireVolumeError error = nullptr;
  PipeWireVolumeChanged changed = nullptr;
  pw_metadata *metadata = nullptr;
  spa_hook metadataListener = {};
  std::uint32_t metadataId = PW_ID_ANY;
  std::uint64_t previousSerial = 0;
  std::string statusKey;
  std::string status;
  void *data = nullptr;
  std::uint32_t channels = 2;
  float gain = 1.0F;
  bool muted = true;
  std::map<std::uint32_t, std::unique_ptr<VolumeBinding>> nodes;
};

void PipeWireOutputVolumeDeleter::operator()(PipeWireOutputVolume *volume) const noexcept {
  if (!volume) return;
  volume->nodes.clear();
  if (volume->metadata) {
    spa_hook_remove(&volume->metadataListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(volume->metadata));
  }
  if (volume->registry) {
    spa_hook_remove(&volume->listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(volume->registry));
  }
  delete volume;
}

void preparePipeWireOutputVolume(PipeWireOutputVolume &volume) {
  for (const auto &[id, binding] : volume.nodes) volume.previousSerial = std::max(volume.previousSerial, binding->serial);
  volume.status.clear();
}

bool pipeWireOutputVolumeReady(const PipeWireOutputVolume &volume, bool multiple) {
  if (!multiple) return volume.nodes.empty() && volume.status == "restored";
  auto serial = volume.previousSerial;
  for (const auto &[id, binding] : volume.nodes) serial = std::max(serial, binding->serial);
  return serial > volume.previousSerial && volume.status == "ready:" + std::to_string(serial);
}

static bool applyVolume(const PipeWireOutputVolume &volume, pw_node *node) {
  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  auto gains = std::array<float, 16>{};
  std::fill_n(gains.begin(), volume.channels, volume.gain);
  const auto *props = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_volume, SPA_POD_Float(1.0F),
      SPA_PROP_mute, SPA_POD_Bool(volume.muted),
      SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, volume.channels, gains.data())));
  return pw_node_set_param(node, SPA_PARAM_Props, 0, props) >= 0;
}

bool setPipeWireOutputVolume(PipeWireOutputVolume &volume, std::uint32_t channels, float gain, bool muted) {
  if (channels < 2 || channels > 16 || !std::isfinite(gain) || gain < 0) return false;
  volume.channels = channels; volume.gain = gain; volume.muted = muted;
  for (const auto &[id, binding] : volume.nodes)
    if (!applyVolume(volume, binding->node)) return false;
  return true;
}

PipeWireOutputVolumePtr createPipeWireOutputVolume(pw_core *core, const std::string &name,
    PipeWireVolumeError error, PipeWireVolumeChanged changed, void *data) {
  auto volume = PipeWireOutputVolumePtr(new PipeWireOutputVolume());
  volume->name = name; volume->error = error; volume->changed = changed; volume->data = data;
  volume->statusKey = "pipetune.volume." + name.substr(0, name.size() - std::strlen(".distribution"));
  volume->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (!volume->registry || !error || !changed) return {};
  static const auto events = [] {
    auto value = pw_registry_events{}; value.version = PW_VERSION_REGISTRY_EVENTS;
    value.global = [](void *data, std::uint32_t id, std::uint32_t, const char *type,
        std::uint32_t version, const spa_dict *props) {
      auto &volume = *static_cast<PipeWireOutputVolume *>(data);
      if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && props && !volume.metadata) {
        const auto *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
        if (!name || std::strcmp(name, "default") != 0) return;
        volume.metadata = static_cast<pw_metadata *>(pw_registry_bind(volume.registry, id, type,
            std::min(version, std::uint32_t{PW_VERSION_METADATA}), 0));
        if (!volume.metadata) return volume.error("cannot observe physical-output gain acknowledgments", volume.data);
        volume.metadataId = id;
        static const auto events = [] {
          auto value = pw_metadata_events{}; value.version = PW_VERSION_METADATA_EVENTS;
          value.property = [](void *data, std::uint32_t subject, const char *key, const char *, const char *text) {
            auto &volume = *static_cast<PipeWireOutputVolume *>(data);
            if (subject != 0 || (key && key != volume.statusKey)) return 0;
            volume.status = text ? text : "";
            if (volume.status.starts_with("error:")) volume.error(volume.status.substr(6), volume.data);
            volume.changed(volume.data);
            return 0;
          };
          return value;
        }();
        pw_metadata_add_listener(volume.metadata, &volume.metadataListener, &events, &volume);
        return;
      }
      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
      const auto *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (!name || name != volume.name) return;
      auto binding = std::make_unique<VolumeBinding>();
      if (const auto *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL))
        std::from_chars(serial, serial + std::strlen(serial), binding->serial);
      binding->node = static_cast<pw_node *>(pw_registry_bind(volume.registry, id, type,
          std::min(version, std::uint32_t{PW_VERSION_NODE}), 0));
      if (!binding->node) return volume.error("cannot bind mixed-output volume", volume.data);
      static const auto proxyEvents = [] {
        auto value = pw_proxy_events{}; value.version = PW_VERSION_PROXY_EVENTS;
        value.error = [](void *data, int, int, const char *message) {
          auto &volume = *static_cast<PipeWireOutputVolume *>(data);
          volume.error(std::string("cannot set mixed-output volume: ") + message, volume.data);
        };
        return value;
      }();
      pw_proxy_add_listener(reinterpret_cast<pw_proxy *>(binding->node), &binding->listener, &proxyEvents, &volume);
      if (!applyVolume(volume, binding->node)) volume.error("cannot set mixed-output volume", volume.data);
      volume.nodes[id] = std::move(binding);
      volume.changed(volume.data);
    };
    value.global_remove = [](void *data, std::uint32_t id) {
      auto &volume = *static_cast<PipeWireOutputVolume *>(data);
      volume.nodes.erase(id);
      if (volume.metadataId == id) {
        spa_hook_remove(&volume.metadataListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(volume.metadata));
        volume.metadata = nullptr;
        volume.metadataId = PW_ID_ANY;
        volume.status.clear();
      }
      volume.changed(volume.data);
    };
    return value;
  }();
  pw_registry_add_listener(volume->registry, &volume->listener, &events, volume.get());
  return volume;
}

} // namespace pipetune
