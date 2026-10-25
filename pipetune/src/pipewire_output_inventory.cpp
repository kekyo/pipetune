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
#include <spa/param/profile.h>
#include <spa/param/route.h>
#include <spa/param/props.h>
#include <spa/pod/iter.h>
#include <spa/pod/builder.h>
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
#include <tuple>

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
  bool canWrite = false;
  struct CardProfile {
    std::int32_t index = -1;
    std::string name;
    std::string description;
    bool available = false;
    std::uint32_t outputNodes = 0;
    bool hasClasses = false;
  };
  std::vector<CardProfile> profiles;
  std::int32_t activeProfile = -1;
  std::vector<PipeWireInputRouteSnapshot> inputRoutes;
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
  std::map<std::string, std::vector<OutputDeviceDescription>> observedLayouts;
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

static OutputCardIdentity cardIdentity(const InventoryProperties &properties) {
  auto card = OutputCardIdentity{};
  card.api = property(properties, "device.api");
  card.location = property(properties, "device.bus-path");
  if (card.location.empty()) card.location = property(properties, "device.name");
  card.vendor = property(properties, "device.vendor.id");
  card.product = property(properties, "device.product.id");
  card.serial = property(properties, "device.serial");
  return card;
}

static std::string cardKey(const OutputCardIdentity &card, std::string_view profile) {
  return card.api + '\0' + card.location + '\0' + card.vendor + '\0' + card.product + '\0' +
      card.serial + '\0' + std::string(profile);
}

static std::string outputProfileKey(std::string_view name) {
  if (name.starts_with("input:")) return {};
  if (name.starts_with("output:")) {
    name.remove_prefix(7);
    const auto end = name.find('+');
    if (end != std::string_view::npos && !name.substr(end).starts_with("+input:")) return {};
    return std::string(name.substr(0, end));
  }
  if (name == "pro-audio") return "pro-audio";
  return {};
}

static std::string_view inputProfilePart(std::string_view name) {
  if (name.starts_with("output:")) {
    const auto separator = name.find("+input:");
    return separator == std::string_view::npos ? std::string_view{} : name.substr(separator + 1);
  }
  if (name.starts_with("input:")) return name;
  return {};
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
  for (const auto key : {"node.pipetune.internal", "node.pipetune.aggregate", "node.pipetune.public-sink", "node.pipetune.public-input",
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
  if ((info->change_mask & PW_DEVICE_CHANGE_MASK_PARAMS) != 0 && device.canEnumerate) {
    device.profiles.clear();
    device.activeProfile = -1;
    device.inputRoutes.clear();
    for (auto index = std::uint32_t{0}; index < info->n_params; ++index) {
      const auto &parameter = info->params[index];
      if ((parameter.flags & SPA_PARAM_INFO_READ) == 0 ||
          (parameter.id != SPA_PARAM_EnumProfile && parameter.id != SPA_PARAM_Profile &&
           parameter.id != SPA_PARAM_Route)) continue;
      pw_device_enum_params(reinterpret_cast<pw_device *>(device.proxy), 0,
          parameter.id, 0, UINT32_MAX, nullptr);
    }
  }
  scheduleSnapshot(*device.owner);
}

static void deviceParam(void *data, int, std::uint32_t id, std::uint32_t,
                        std::uint32_t, const spa_pod *param) {
  auto &device = *static_cast<InventoryObject *>(data);
  if (param == nullptr) return;
  if (id == SPA_PARAM_Route) {
    auto direction = std::uint32_t{SPA_ID_INVALID};
    auto index = std::int32_t{-1};
    auto logicalDevice = std::int32_t{-1};
    auto *props = static_cast<spa_pod *>(nullptr);
    if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, nullptr,
        SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction),
        SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
        SPA_PARAM_ROUTE_device, SPA_POD_Int(&logicalDevice),
        SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0 ||
        direction != SPA_DIRECTION_INPUT || index < 0 || logicalDevice < 0) return;
    auto route = PipeWireInputRouteSnapshot{.index = index, .device = logicalDevice, .props = {}};
    if (props != nullptr) {
      const auto size = SPA_POD_SIZE(props);
      if (size > 0 && size <= 65536) {
        route.props.resize(size);
        std::memcpy(route.props.data(), props, size);
      }
    }
    const auto existing = std::find_if(device.inputRoutes.begin(), device.inputRoutes.end(),
        [&](const auto &item) { return item.device == route.device; });
    if (existing == device.inputRoutes.end()) device.inputRoutes.push_back(std::move(route));
    else *existing = std::move(route);
    std::sort(device.inputRoutes.begin(), device.inputRoutes.end(), [](const auto &left, const auto &right) {
      return left.device < right.device;
    });
    scheduleSnapshot(*device.owner);
    return;
  }
  if (id != SPA_PARAM_EnumProfile && id != SPA_PARAM_Profile) return;
  auto index = std::int32_t{-1};
  const auto *name = static_cast<const char *>(nullptr);
  const auto *description = static_cast<const char *>(nullptr);
  auto available = std::uint32_t{SPA_PARAM_AVAILABILITY_unknown};
  auto *classes = static_cast<spa_pod *>(nullptr);
  if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, nullptr,
      SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
      SPA_PARAM_PROFILE_name, SPA_POD_OPT_String(&name),
      SPA_PARAM_PROFILE_description, SPA_POD_OPT_String(&description),
      SPA_PARAM_PROFILE_available, SPA_POD_OPT_Id(&available),
      SPA_PARAM_PROFILE_classes, SPA_POD_OPT_Pod(&classes)) < 0 || index < 0) return;
  if (id == SPA_PARAM_Profile) device.activeProfile = index;
  else if (name != nullptr && name[0] != '\0') {
    auto profile = InventoryObject::CardProfile{};
    profile.index = index;
    profile.name = name;
    profile.description = description == nullptr ? name : description;
    profile.available = available != SPA_PARAM_AVAILABILITY_no;
    if (classes != nullptr && spa_pod_is_struct(classes)) {
      auto parser = spa_pod_parser{};
      spa_pod_parser_pod(&parser, classes);
      auto frame = spa_pod_frame{};
      auto count = std::int32_t{0};
      if (spa_pod_parser_push_struct(&parser, &frame) == 0 &&
          spa_pod_parser_get(&parser, SPA_POD_Int(&count), 0) >= 0 && count >= 0 && count <= 64) {
        auto complete = true;
        for (auto item = 0; item < count; ++item) {
          const auto *className = static_cast<const char *>(nullptr);
          const auto *propertyName = static_cast<const char *>(nullptr);
          auto nodes = std::int32_t{0};
          auto *devices = static_cast<spa_pod *>(nullptr);
          auto classFrame = spa_pod_frame{};
          if (spa_pod_parser_push_struct(&parser, &classFrame) < 0) {
            complete = false;
            break;
          }
          const auto parsed = spa_pod_parser_get(&parser, SPA_POD_String(&className), SPA_POD_Int(&nodes),
              SPA_POD_String(&propertyName), SPA_POD_Pod(&devices), 0);
          spa_pod_parser_pop(&parser, &classFrame);
          if (parsed < 0 || className == nullptr) {
            complete = false;
            break;
          }
          static_cast<void>(propertyName);
          static_cast<void>(devices);
          if (className != nullptr && std::strcmp(className, "Audio/Sink") == 0 && nodes > 0)
            profile.outputNodes += static_cast<std::uint32_t>(nodes);
        }
        profile.hasClasses = complete;
        if (!complete) profile.outputNodes = 0;
      }
    }
    device.profiles.push_back(std::move(profile));
  }
  scheduleSnapshot(*device.owner);
}

static const pw_node_events nodeEvents = {.version = PW_VERSION_NODE_EVENTS, .info = nodeInfo, .param = nodeParam};
static const pw_device_events deviceEvents = {.version = PW_VERSION_DEVICE_EVENTS, .info = deviceInfo, .param = deviceParam};

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
  object->canWrite = (permissions & (PW_PERM_W | PW_PERM_X)) == (PW_PERM_W | PW_PERM_X);
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
  for (const auto &[deviceId, device] : inventory.devices) {
    const auto card = cardIdentity(device->properties);
    if (card.api.empty() || card.location.empty()) continue;
    const auto active = std::find_if(device->profiles.begin(), device->profiles.end(), [&](const auto &profile) {
      return profile.index == device->activeProfile;
    });
    const auto activeName = active == device->profiles.end() ? std::string{} : active->name;
    if (!activeName.empty()) {
      auto activeOutputs = std::vector<OutputDeviceDescription>{};
      for (const auto &[nodeId, node] : inventory.nodes) {
        const auto linked = unsignedValue(property(node->properties, "device.id"));
        if (linked && *linked == deviceId && isOutput(node->properties)) {
          auto output = describeOutput(nodeId, *node, inventory).device;
          if (!output.channelPositions.empty()) activeOutputs.push_back(std::move(output));
        }
      }
      if (!activeOutputs.empty()) inventory.observedLayouts[cardKey(card, activeName)] = std::move(activeOutputs);
    }
    for (const auto &profile : device->profiles) {
      auto entry = AvailableOutputProfile{};
      entry.card = card;
      entry.cardProfileName = profile.name;
      entry.outputKey = outputProfileKey(profile.name);
      entry.description = profile.description;
      entry.available = profile.available;
      entry.active = profile.index == device->activeProfile;
      const auto managedActive = activeName.starts_with("output:") || activeName.starts_with("input:") || activeName == "off";
      const auto managedCandidate = profile.name.starts_with("output:") ||
          profile.name.starts_with("input:") || profile.name == "off";
      entry.preservesInput = entry.active ||
          (managedActive && managedCandidate && inputProfilePart(profile.name) == inputProfilePart(activeName));
      const auto observed = inventory.observedLayouts.find(cardKey(card, profile.name));
      if (observed != inventory.observedLayouts.end()) entry.observedOutputs = observed->second;
      snapshot.profiles.push_back(std::move(entry));
    }
  }
  std::sort(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &left, const auto &right) {
    return std::tie(left.card.api, left.card.location, left.card.vendor, left.card.product, left.card.serial,
               left.cardProfileName) <
        std::tie(right.card.api, right.card.location, right.card.vendor, right.card.product, right.card.serial,
               right.cardProfileName);
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

std::string setPipeWireOutputProfile(
    PipeWireOutputInventory &inventory, const OutputProfileSelection &selection) {
  auto *target = static_cast<InventoryObject *>(nullptr);
  for (const auto &[id, device] : inventory.devices) {
    static_cast<void>(id);
    if (cardIdentity(device->properties) != selection.card) continue;
    if (target != nullptr) return "multiple cards match the selected profile identity";
    target = device.get();
  }
  if (target == nullptr) return "the selected output card is unavailable";
  if (!target->canWrite) return "the selected output card does not permit profile changes";
  const auto candidate = std::find_if(target->profiles.begin(), target->profiles.end(), [&](const auto &profile) {
    return profile.name == selection.cardProfileName;
  });
  if (candidate == target->profiles.end() || !candidate->available)
    return "the selected output profile is unavailable";
  if (outputProfileKey(candidate->name) != selection.outputKey && !selection.allowMissingForRestore)
    return "the selected card profile does not match the output intent";
  if (candidate->index == target->activeProfile) return {};
  const auto active = std::find_if(target->profiles.begin(), target->profiles.end(), [&](const auto &profile) {
    return profile.index == target->activeProfile;
  });
  if (active == target->profiles.end() ||
      (!active->name.starts_with("output:") && !active->name.starts_with("input:") &&
       active->name != "off") ||
      (!candidate->name.starts_with("output:") && !candidate->name.starts_with("input:") &&
       candidate->name != "off") ||
      inputProfilePart(candidate->name) != inputProfilePart(active->name))
    return "the selected card profile would change the input profile";
  auto storage = std::array<std::uint8_t, 256>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto *parameter = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder,
      SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
      SPA_PARAM_PROFILE_index, SPA_POD_Int(candidate->index),
      SPA_PARAM_PROFILE_save, SPA_POD_Bool(false)));
  if (parameter == nullptr) return "cannot build the output profile request";
  const auto result = pw_device_set_param(reinterpret_cast<pw_device *>(target->proxy), SPA_PARAM_Profile, 0, parameter);
  return result < 0 ? "PipeWire rejected the output profile request: " + std::to_string(result) : std::string{};
}

std::optional<std::uint32_t> pipeWireOutputProfileNodeCount(
    const PipeWireOutputInventory &inventory, const OutputProfileSelection &selection) {
  for (const auto &[id, device] : inventory.devices) {
    static_cast<void>(id);
    if (cardIdentity(device->properties) != selection.card) continue;
    const auto found = std::find_if(device->profiles.begin(), device->profiles.end(), [&](const auto &profile) {
      return profile.name == selection.cardProfileName;
    });
    if (found != device->profiles.end() && found->hasClasses) return found->outputNodes;
  }
  return {};
}

std::vector<PipeWireInputRouteSnapshot> pipeWireInputRoutes(
    const PipeWireOutputInventory &inventory, const OutputCardIdentity &card) {
  for (const auto &[id, device] : inventory.devices) {
    static_cast<void>(id);
    if (cardIdentity(device->properties) == card) return device->inputRoutes;
  }
  return {};
}

std::string restorePipeWireInputRoutes(PipeWireOutputInventory &inventory,
    const OutputCardIdentity &card, std::span<const PipeWireInputRouteSnapshot> routes) {
  auto *target = static_cast<InventoryObject *>(nullptr);
  for (const auto &[id, device] : inventory.devices) {
    static_cast<void>(id);
    if (cardIdentity(device->properties) != card) continue;
    if (target != nullptr) return "multiple cards match the saved input route identity";
    target = device.get();
  }
  if (target == nullptr) return "the input card is unavailable";
  if (!target->canWrite) return "the input card does not permit route restoration";
  for (const auto &route : routes) {
    if (route.index < 0 || route.device < 0 || route.props.size() > 65536)
      return "invalid saved input route";
    const auto current = std::find_if(target->inputRoutes.begin(), target->inputRoutes.end(), [&](const auto &item) {
      return item.device == route.device;
    });
    if (current != target->inputRoutes.end() && *current == route) continue;
    auto storage = std::array<std::uint8_t, 66048>{};
    auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
    auto *parameter = static_cast<spa_pod *>(nullptr);
    if (route.props.empty()) {
      parameter = static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
          SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
          SPA_PARAM_ROUTE_index, SPA_POD_Int(route.index),
          SPA_PARAM_ROUTE_device, SPA_POD_Int(route.device),
          SPA_PARAM_ROUTE_save, SPA_POD_Bool(false)));
    } else {
      alignas(8) auto aligned = std::array<std::uint8_t, 65536>{};
      std::memcpy(aligned.data(), route.props.data(), route.props.size());
      parameter = static_cast<spa_pod *>(spa_pod_builder_add_object(&builder,
          SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
          SPA_PARAM_ROUTE_index, SPA_POD_Int(route.index),
          SPA_PARAM_ROUTE_device, SPA_POD_Int(route.device),
          SPA_PARAM_ROUTE_save, SPA_POD_Bool(false),
          SPA_PARAM_ROUTE_props, SPA_POD_Pod(reinterpret_cast<const spa_pod *>(aligned.data()))));
    }
    if (parameter == nullptr) return "cannot build the input route restoration request";
    const auto result = pw_device_set_param(reinterpret_cast<pw_device *>(target->proxy), SPA_PARAM_Route, 0, parameter);
    if (result < 0) return "PipeWire rejected input route restoration: " + std::to_string(result);
  }
  return {};
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
