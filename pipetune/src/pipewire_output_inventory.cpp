/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_inventory.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/raw-types.h>
#include <spa/param/format.h>
#include <spa/param/port-config.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <map>
#include <optional>
#include <string_view>

namespace pipetune {

using InventoryProperties = std::map<std::string, std::string, std::less<>>;

struct InventoryObject {
  PipeWireOutputInventory *owner = nullptr;
  pw_proxy *proxy = nullptr;
  spa_hook listener = {};
  InventoryProperties properties;
  // Each parameter family is replaced when the node announces a change.
  std::map<std::uint32_t, std::vector<std::vector<std::string>>> layouts;
  bool canEnumerate = false;
  OutputVolumeState volume;

  ~InventoryObject() {
    if (proxy != nullptr) {
      spa_hook_remove(&listener);
      pw_proxy_destroy(proxy);
    }
  }
};

struct PipeWireOutputInventory {
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook registryListener = {};
  spa_hook coreListener = {};
  std::map<std::uint32_t, std::unique_ptr<InventoryObject>> nodes;
  std::map<std::uint32_t, std::unique_ptr<InventoryObject>> devices;
  OutputInventoryCallback callback = nullptr;
  void *userData = nullptr;
  int pending = 0;
  std::uint32_t defaultMetadataId = PW_ID_ANY;
  std::uint64_t policyGeneration = 0;
};

static void updateProperties(InventoryProperties &properties, const spa_dict *dictionary) {
  if (dictionary == nullptr) return;
  for (auto index = std::uint32_t{0}; index < dictionary->n_items; ++index) {
    const auto &item = dictionary->items[index];
    if (item.value == nullptr) properties.erase(item.key);
    else properties[item.key] = item.value;
  }
}

static std::string property(const InventoryProperties &properties, std::string_view key) {
  const auto found = properties.find(key);
  return found == properties.end() ? std::string{} : found->second;
}

static std::optional<std::uint32_t> unsignedValue(std::string_view value) {
  auto number = std::uint32_t{0};
  const auto parsed = std::from_chars(value.data(), value.data() + value.size(), number);
  if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) return {};
  return number;
}

static std::string channelName(std::uint32_t position) {
  for (auto *type = spa_type_audio_channel; type->name != nullptr; ++type) {
    if (type->type == position) {
      const auto *separator = std::strrchr(type->name, ':');
      return separator == nullptr ? type->name : separator + 1;
    }
  }
  return "UNKNOWN";
}

static std::vector<std::string> formatChannels(const spa_pod *format) {
  if (format == nullptr || !spa_pod_is_object_type(format, SPA_TYPE_OBJECT_Format)) return {};
  const auto *channels = spa_pod_find_prop(format, nullptr, SPA_FORMAT_AUDIO_channels);
  auto count = std::int32_t{0};
  // EnumFormat can advertise a range. Its preferred value is not a committed
  // profile layout, so accept only a fixed channel count.
  if (channels == nullptr || spa_pod_get_int(&channels->value, &count) < 0 ||
      count <= 0 || static_cast<std::uint32_t>(count) > SPA_AUDIO_MAX_CHANNELS) return {};
  const auto *positions = spa_pod_find_prop(format, nullptr, SPA_FORMAT_AUDIO_position);
  auto values = std::array<std::uint32_t, SPA_AUDIO_MAX_CHANNELS>{};
  if (positions == nullptr) return std::vector<std::string>(count, "UNKNOWN");
  if (spa_pod_copy_array(&positions->value, SPA_TYPE_Id, values.data(), values.size()) !=
      static_cast<std::uint32_t>(count)) return {};
  auto result = std::vector<std::string>{};
  for (auto index = 0; index < count; ++index) result.push_back(channelName(values[index]));
  return result;
}

static std::vector<std::string> propertyChannels(const InventoryProperties &properties) {
  auto positions = std::string_view{};
  const auto found = properties.find("audio.position");
  if (found != properties.end()) positions = found->second;
  auto result = std::vector<std::string>{};
  // PipeWire properties use SPA-JSON arrays or comma-separated position names.
  // Names contain no quoting escapes; malformed/unknown names are not guessed.
  constexpr auto delimiters = "[] ,\"\t\r\n";
  while (!positions.empty()) {
    const auto start = positions.find_first_not_of(delimiters);
    if (start == std::string_view::npos) break;
    positions.remove_prefix(start);
    const auto end = positions.find_first_of(delimiters);
    const auto name = positions.substr(0, end);
    auto known = false;
    for (auto *type = spa_type_audio_channel; type->name != nullptr; ++type) {
      const auto *separator = std::strrchr(type->name, ':');
      if (name == (separator == nullptr ? type->name : separator + 1)) {
        known = true;
        break;
      }
    }
    if (!known || result.size() >= SPA_AUDIO_MAX_CHANNELS) return {};
    result.emplace_back(name);
    if (end == std::string_view::npos) break;
    positions.remove_prefix(end);
  }
  const auto count = unsignedValue(property(properties, "audio.channels"));
  if (count && (*count == 0 || *count > SPA_AUDIO_MAX_CHANNELS)) return {};
  if (count && result.empty()) return std::vector<std::string>(*count, "UNKNOWN");
  if (count && *count != result.size()) return {};
  return result;
}

static bool isOutput(const InventoryProperties &properties) {
  if (property(properties, "media.class") != "Audio/Sink") return false;
  for (const auto key : {"node.pipetune.internal", "node.pipetune.aggregate", "node.pipetune.public-input",
                         "wireplumber.is-endpoint"}) {
    if (property(properties, key) == "true") return false;
  }
  return !property(properties, "node.name").empty();
}

static AvailableOutput describeOutput(std::uint32_t id, const InventoryObject &node,
                                     const PipeWireOutputInventory &inventory) {
  auto properties = InventoryProperties{};
  const auto deviceId = unsignedValue(property(node.properties, "device.id"));
  if (deviceId) {
    const auto device = inventory.devices.find(*deviceId);
    if (device != inventory.devices.end()) properties = device->second->properties;
  }
  for (const auto &[key, value] : node.properties) properties[key] = value;
  auto output = AvailableOutput{};
  output.nodeId = id;
  const auto serial = property(node.properties, "object.serial");
  const auto parsedSerial = std::from_chars(serial.data(), serial.data() + serial.size(), output.nodeSerial);
  if (parsedSerial.ec != std::errc{} || parsedSerial.ptr != serial.data() + serial.size()) output.nodeSerial = 0;
  output.nodeName = property(properties, "node.name");
  auto &device = output.device;
  device.name = property(properties, "node.description");
  if (device.name.empty()) device.name = property(properties, "node.nick");
  if (device.name.empty()) device.name = output.nodeName;
  device.profile = property(properties, "device.profile.name");
  const auto outputProfile = property(properties, "node.device.profile.name");
  if (!outputProfile.empty()) device.profile = outputProfile;
  auto &identity = device.identity;
  identity.api = property(properties, "device.api");
  if (identity.api.empty()) identity.api = "node";
  identity.location = property(properties, "device.bus-path");
  if (identity.location.empty()) identity.location = property(properties, "device.name");
  if (identity.location.empty() && identity.api != "alsa") identity.location = output.nodeName;
  identity.vendor = property(properties, "device.vendor.id");
  identity.product = property(properties, "device.product.id");
  identity.serial = property(properties, "device.serial");
  if (identity.api == "alsa") {
    const auto pcm = unsignedValue(property(properties, "alsa.device"));
    const auto subdevice = unsignedValue(property(properties, "alsa.subdevice"));
    if (pcm && subdevice) identity.port = "pcm:" + std::to_string(*pcm) + ":" + std::to_string(*subdevice);
  } else {
    identity.port = output.nodeName;
  }
  for (const auto parameter : {SPA_PARAM_PortConfig, SPA_PARAM_Format, SPA_PARAM_EnumFormat}) {
    const auto found = node.layouts.find(parameter);
    if (found == node.layouts.end() || found->second.empty()) continue;
    const auto &layouts = found->second;
    if (std::all_of(layouts.begin(), layouts.end(), [&](const auto &layout) { return layout == layouts.front(); })) {
      device.channelPositions = layouts.front();
      break;
    }
  }
  if (device.channelPositions.empty()) device.channelPositions = propertyChannels(node.properties);
  return output;
}

static void scheduleSnapshot(PipeWireOutputInventory &inventory) {
  inventory.pending = pw_core_sync(inventory.core, PW_ID_CORE, 0);
}

static void nodeInfo(void *data, const pw_node_info *info) {
  auto &node = *static_cast<InventoryObject *>(data);
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0) updateProperties(node.properties, info->props);
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PARAMS) != 0 && node.canEnumerate) {
    node.layouts.clear();
    node.volume = {};
    for (auto index = std::uint32_t{0}; index < info->n_params; ++index) {
      const auto &parameter = info->params[index];
      if ((parameter.flags & SPA_PARAM_INFO_READ) == 0 ||
          (parameter.id != SPA_PARAM_PortConfig && parameter.id != SPA_PARAM_Format &&
           parameter.id != SPA_PARAM_EnumFormat && parameter.id != SPA_PARAM_Props)) continue;
      pw_node_enum_params(reinterpret_cast<pw_node *>(node.proxy), 0, parameter.id, 0, UINT32_MAX, nullptr);
    }
  }
  scheduleSnapshot(*node.owner);
}

static void nodeParam(void *data, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod *param) {
  auto &node = *static_cast<InventoryObject *>(data);
  if (id == SPA_PARAM_Props) {
    // A single enumeration can contain separate mixer and device parameter
    // objects. Merge the controls reported by all of them; nodeInfo resets
    // the snapshot before enumeration so an old profile cannot leave stale values.
    auto &volume = node.volume;
    if (param != nullptr && spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Props)) {
      if (const auto *property = spa_pod_find_prop(param, nullptr, SPA_PROP_mute)) {
        volume.muted.reset();
        auto muted = false;
        if (spa_pod_get_bool(&property->value, &muted) >= 0) volume.muted = muted;
      }
      if (const auto *property = spa_pod_find_prop(param, nullptr, SPA_PROP_volume)) {
        volume.volume.reset();
        auto gain = 0.0F;
        if (spa_pod_get_float(&property->value, &gain) >= 0 && std::isfinite(gain) && gain >= 0) volume.volume = gain;
      }
      if (const auto *property = spa_pod_find_prop(param, nullptr, SPA_PROP_channelVolumes)) {
        volume.channelVolumes.clear();
        auto gains = std::array<float, SPA_AUDIO_MAX_CHANNELS>{};
        const auto validArray = spa_pod_is_array(&property->value) &&
            SPA_POD_ARRAY_VALUE_SIZE(&property->value) == sizeof(float) &&
            SPA_POD_ARRAY_N_VALUES(&property->value) <= gains.size();
        const auto count = validArray ? spa_pod_copy_array(&property->value, SPA_TYPE_Float, gains.data(), gains.size()) : 0U;
        if (count > 0 && std::all_of(gains.begin(), gains.begin() + count,
            [](float gain) { return std::isfinite(gain) && gain >= 0; }))
          volume.channelVolumes.assign(gains.begin(), gains.begin() + count);
      }
    }
    return;
  }
  auto *format = param;
  if (id == SPA_PARAM_PortConfig) {
    auto direction = std::uint32_t{SPA_ID_INVALID};
    auto *portFormat = static_cast<spa_pod *>(nullptr);
    if (param == nullptr || spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamPortConfig, nullptr,
        SPA_PARAM_PORT_CONFIG_direction, SPA_POD_Id(&direction),
        SPA_PARAM_PORT_CONFIG_format, SPA_POD_OPT_Pod(&portFormat)) < 0 || direction != SPA_DIRECTION_INPUT) return;
    format = portFormat;
  }
  auto layout = formatChannels(format);
  if (!layout.empty()) node.layouts[id].push_back(std::move(layout));
}

static void deviceInfo(void *data, const pw_device_info *info) {
  auto &device = *static_cast<InventoryObject *>(data);
  if ((info->change_mask & PW_DEVICE_CHANGE_MASK_PROPS) != 0) updateProperties(device.properties, info->props);
  scheduleSnapshot(*device.owner);
}

static const pw_node_events nodeEvents = {.version = PW_VERSION_NODE_EVENTS, .info = nodeInfo, .param = nodeParam};
static const pw_device_events deviceEvents = {.version = PW_VERSION_DEVICE_EVENTS, .info = deviceInfo, .param = nullptr};

static void globalAdded(void *data, std::uint32_t id, std::uint32_t permissions,
                        const char *type, std::uint32_t version, const spa_dict *props) {
  auto &inventory = *static_cast<PipeWireOutputInventory *>(data);
  if ((permissions & PW_PERM_R) == 0) return;
  if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && props != nullptr) {
    const auto *name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
    if (name != nullptr && std::strcmp(name, "default") == 0) inventory.defaultMetadataId = id;
    return;
  }
  const auto isNode = std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0;
  const auto isDevice = std::strcmp(type, PW_TYPE_INTERFACE_Device) == 0;
  if (!isNode && !isDevice) return;
  auto object = std::make_unique<InventoryObject>();
  object->owner = &inventory;
  object->canEnumerate = (permissions & PW_PERM_X) != 0;
  updateProperties(object->properties, props);
  if (isNode && !isOutput(object->properties)) return;
  object->proxy = static_cast<pw_proxy *>(pw_registry_bind(inventory.registry, id, type,
      std::min<std::uint32_t>(version, isNode ? PW_VERSION_NODE : PW_VERSION_DEVICE), 0));
  if (object->proxy == nullptr) return;
  if (isNode) {
    pw_node_add_listener(reinterpret_cast<pw_node *>(object->proxy), &object->listener, &nodeEvents, object.get());
    inventory.nodes[id] = std::move(object);
  } else {
    pw_device_add_listener(reinterpret_cast<pw_device *>(object->proxy), &object->listener, &deviceEvents, object.get());
    inventory.devices[id] = std::move(object);
  }
  scheduleSnapshot(inventory);
}

static void globalRemoved(void *data, std::uint32_t id) {
  auto &inventory = *static_cast<PipeWireOutputInventory *>(data);
  const auto removedNode = inventory.nodes.erase(id);
  const auto removedDevice = inventory.devices.erase(id);
  const auto removedPolicy = id == inventory.defaultMetadataId;
  if (removedPolicy) {
    inventory.defaultMetadataId = PW_ID_ANY;
    ++inventory.policyGeneration;
  }
  if (removedNode != 0 || removedDevice != 0 || removedPolicy) scheduleSnapshot(inventory);
}

static void coreDone(void *data, std::uint32_t id, int sequence) {
  auto &inventory = *static_cast<PipeWireOutputInventory *>(data);
  if (id != PW_ID_CORE || sequence != inventory.pending) return;
  auto snapshot = OutputInventoryResult{};
  for (const auto &[nodeId, node] : inventory.nodes) {
    if (!isOutput(node->properties)) continue;
    auto output = describeOutput(nodeId, *node, inventory);
    if (output.nodeSerial != 0) {
      auto volume = node->volume;
      volume.nodeSerial = output.nodeSerial;
      snapshot.volumes.push_back(std::move(volume));
    }
    snapshot.outputs.push_back(std::move(output));
  }
  std::sort(snapshot.outputs.begin(), snapshot.outputs.end(), [](const auto &left, const auto &right) {
    return left.nodeName < right.nodeName || (left.nodeName == right.nodeName && left.nodeId < right.nodeId);
  });
  inventory.callback(snapshot, inventory.userData);
}

static void coreError(void *data, std::uint32_t id, int, int code, const char *message) {
  // Method errors for disappearing resources can also be reported on the
  // core. They do not mean that the connection or its registry was lost.
  if (id != PW_ID_CORE || code != -EPIPE) return;
  auto &inventory = *static_cast<PipeWireOutputInventory *>(data);
  const auto result = OutputInventoryResult{.outputs = {}, .error =
      std::string("PipeWire output enumeration failed: ") + (message == nullptr ? "connection lost" : message)};
  inventory.callback(result, inventory.userData);
}

PipeWireOutputInventoryPtr observePipeWireOutputs(
    pw_core *core, OutputInventoryCallback callback, void *userData) {
  if (core == nullptr || callback == nullptr) return {};
  auto inventory = PipeWireOutputInventoryPtr(new PipeWireOutputInventory{});
  inventory->core = core;
  inventory->callback = callback;
  inventory->userData = userData;
  inventory->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (inventory->registry == nullptr) return {};
  static const auto coreEvents = [] {
    auto events = pw_core_events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.done = coreDone;
    events.error = coreError;
    return events;
  }();
  static const pw_registry_events registryEvents = {
      .version = PW_VERSION_REGISTRY_EVENTS, .global = globalAdded, .global_remove = globalRemoved};
  pw_core_add_listener(core, &inventory->coreListener, &coreEvents, inventory.get());
  pw_registry_add_listener(inventory->registry, &inventory->registryListener, &registryEvents, inventory.get());
  scheduleSnapshot(*inventory);
  return inventory;
}

std::uint64_t pipeWireOutputPolicyGeneration(const PipeWireOutputInventory &inventory) noexcept {
  return inventory.policyGeneration;
}

void PipeWireOutputInventoryDeleter::operator()(PipeWireOutputInventory *inventory) const noexcept {
  if (inventory == nullptr) return;
  inventory->nodes.clear();
  inventory->devices.clear();
  if (inventory->registry != nullptr) {
    spa_hook_remove(&inventory->registryListener);
    spa_hook_remove(&inventory->coreListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(inventory->registry));
  }
  delete inventory;
}

} // namespace pipetune
