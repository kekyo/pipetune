/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_volume.h"

#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <map>

namespace pipetune {

struct VolumeBinding {
  pw_node *node = nullptr;
  spa_hook listener = {};
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
  void *data = nullptr;
  std::uint32_t channels = 2;
  float gain = 1.0F;
  bool muted = true;
  std::map<std::uint32_t, std::unique_ptr<VolumeBinding>> nodes;
};

void PipeWireOutputVolumeDeleter::operator()(PipeWireOutputVolume *volume) const noexcept {
  if (!volume) return;
  volume->nodes.clear();
  if (volume->registry) {
    spa_hook_remove(&volume->listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(volume->registry));
  }
  delete volume;
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
    PipeWireVolumeError error, void *data) {
  auto volume = PipeWireOutputVolumePtr(new PipeWireOutputVolume());
  volume->name = name; volume->error = error; volume->data = data;
  volume->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (!volume->registry || !error) return {};
  static const auto events = [] {
    auto value = pw_registry_events{}; value.version = PW_VERSION_REGISTRY_EVENTS;
    value.global = [](void *data, std::uint32_t id, std::uint32_t, const char *type,
        std::uint32_t version, const spa_dict *props) {
      auto &volume = *static_cast<PipeWireOutputVolume *>(data);
      if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props) return;
      const auto *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
      if (!name || name != volume.name) return;
      auto binding = std::make_unique<VolumeBinding>();
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
    };
    value.global_remove = [](void *data, std::uint32_t id) {
      static_cast<PipeWireOutputVolume *>(data)->nodes.erase(id);
    };
    return value;
  }();
  pw_registry_add_listener(volume->registry, &volume->listener, &events, volume.get());
  return volume;
}

} // namespace pipetune
