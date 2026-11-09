/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
// Exercise real observer callbacks with reordered public protocol responses.
#include "../src/pipewire_output_inventory.cpp"
#include <spa/utils/result.h>

#include <iostream>

struct Requests {
  int sequence = 0;
  std::map<std::uint32_t, int> parameters;
};

int main() {
  auto coreMethods = pw_core_methods{};
  coreMethods.version = PW_VERSION_CORE_METHODS;
  coreMethods.sync = [](void *, std::uint32_t, int) { return 123; };
  auto core = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Core, PW_VERSION_CORE, &coreMethods, nullptr);
  auto inventory = pipetune::PipeWireOutputInventory{};
  inventory.core = reinterpret_cast<pw_core *>(&core);
  auto snapshot = pipetune::OutputInventoryResult{};
  inventory.userData = &snapshot;
  inventory.callback = [](const auto &value, void *data) {
    *static_cast<pipetune::OutputInventoryResult *>(data) = value;
  };

  auto requests = Requests{};
  auto methods = pw_device_methods{};
  methods.version = PW_VERSION_DEVICE_METHODS;
  methods.enum_params = [](void *data, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod *) {
    auto &requests = *static_cast<Requests *>(data);
    const auto result = SPA_RESULT_RETURN_ASYNC(++requests.sequence);
    requests.parameters[id] = result;
    return result;
  };
  auto interface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, &methods, &requests);
  auto device = std::make_unique<pipetune::InventoryObject>();
  device->owner = &inventory;
  device->canEnumerate = true;
  device->proxy = reinterpret_cast<pw_proxy *>(&interface);
  device->properties = {{"device.api", "test"}, {"device.name", "generation"}};
  auto *target = device.get();
  inventory.devices.emplace(42, std::move(device));
  auto parameters = std::array<spa_param_info, 3>{};
  for (auto index = 0U; const auto id : {SPA_PARAM_EnumProfile, SPA_PARAM_Profile, SPA_PARAM_Route}) {
    parameters[index].id = id;
    parameters[index++].flags = SPA_PARAM_INFO_READ;
  }
  auto info = pw_device_info{};
  info.change_mask = PW_DEVICE_CHANGE_MASK_PARAMS;
  info.params = parameters.data();
  info.n_params = parameters.size();
  pipetune::deviceInfo(target, &info);
  const auto old = requests.parameters;
  pipetune::deviceInfo(target, &info);
  const auto current = requests.parameters;
  const auto profile = [&](const auto &generation, std::uint32_t id, int index, const char *name) {
    auto bytes = std::array<std::uint8_t, 512>{};
    auto builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    const auto *pod = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamProfile, id, SPA_PARAM_PROFILE_index, SPA_POD_Int(index),
        SPA_PARAM_PROFILE_name, SPA_POD_String(name)));
    pipetune::deviceParam(target, generation.at(id), id, index, index + 1, pod);
  };
  const auto route = [&](const auto &generation, int index) {
    auto bytes = std::array<std::uint8_t, 512>{};
    auto builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    const auto *pod = static_cast<const spa_pod *>(spa_pod_builder_add_object(&builder,
        SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
        SPA_PARAM_ROUTE_direction, SPA_POD_Id(SPA_DIRECTION_INPUT),
        SPA_PARAM_ROUTE_index, SPA_POD_Int(index), SPA_PARAM_ROUTE_device, SPA_POD_Int(1)));
    pipetune::deviceParam(target, generation.at(SPA_PARAM_Route), SPA_PARAM_Route, 0, 1, pod);
  };
  profile(current, SPA_PARAM_EnumProfile, 0, "output:analog+input:mic");
  profile(old, SPA_PARAM_EnumProfile, 0, "output:obsolete+input:mic");
  profile(current, SPA_PARAM_EnumProfile, 1, "output:optical+input:mic");
  profile(current, SPA_PARAM_Profile, 1, "output:optical+input:mic");
  profile(old, SPA_PARAM_Profile, 0, "output:analog+input:mic");
  route(current, 2);
  route(old, 3);
  pipetune::coreDone(&inventory, PW_ID_CORE, inventory.pending);
  const auto active = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &p) { return p.active; });
  const auto routes = pipetune::pipeWireInputRoutes(inventory,
      {.api = "test", .location = "generation", .vendor = {}, .product = {}, .serial = {}});
  target->proxy = nullptr;
  if (snapshot.profiles.size() != 2 || active == snapshot.profiles.end() ||
      active->cardProfileName != "output:optical+input:mic" || routes.size() != 1 || routes[0].index != 2) {
    std::cerr << "late enumeration responses changed the current card profiles or input route\n";
    return 1;
  }
  auto nodeMethods = pw_node_methods{};
  nodeMethods.version = PW_VERSION_NODE_METHODS;
  nodeMethods.enum_params = methods.enum_params;
  auto nodeInterface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &nodeMethods, &requests);
  auto node = std::make_unique<pipetune::InventoryObject>();
  node->owner = &inventory;
  node->canEnumerate = true;
  node->proxy = reinterpret_cast<pw_proxy *>(&nodeInterface);
  node->properties = {{"media.class", "Audio/Sink"}, {"object.serial", "43"},
      {"node.name", "generation.output"}, {"audio.position", "[ FL FR ]"}};
  auto *sink = node.get();
  inventory.nodes.emplace(43, std::move(node));
  auto volumeParameter = spa_param_info{};
  volumeParameter.id = SPA_PARAM_Props;
  volumeParameter.flags = SPA_PARAM_INFO_READ;
  auto nodeInfo = pw_node_info{};
  nodeInfo.change_mask = PW_NODE_CHANGE_MASK_PARAMS;
  nodeInfo.params = &volumeParameter;
  nodeInfo.n_params = 1;
  pipetune::nodeInfo(sink, &nodeInfo);
  const auto oldVolume = requests.parameters.at(SPA_PARAM_Props);
  pipetune::nodeInfo(sink, &nodeInfo);
  const auto currentVolume = requests.parameters.at(SPA_PARAM_Props);
  const auto volume = [&](int sequence, bool hardware, float gain) {
    auto bytes = std::array<std::uint8_t, 512>{};
    auto builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
    const auto *pod = static_cast<const spa_pod *>(hardware ?
        spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_device, SPA_POD_String("test:device")) :
        spa_pod_builder_add_object(&builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_volume, SPA_POD_Float(gain), SPA_PROP_mute, SPA_POD_Bool(false)));
    pipetune::nodeParam(sink, sequence, SPA_PARAM_Props, hardware ? 1 : 0, hardware ? 2 : 1, pod);
  };
  volume(currentVolume, false, 0.5F);
  volume(currentVolume, true, 0.0F);
  volume(oldVolume, false, 0.125F);
  pipetune::coreDone(&inventory, PW_ID_CORE, inventory.pending);
  sink->proxy = nullptr;
  if (snapshot.volumes.size() != 1 || snapshot.volumes[0].volume != 0.5F || snapshot.volumes[0].muted != false) {
    std::cerr << "late controls replaced the current node volume snapshot\n";
    return 2;
  }
  return 0;
}
