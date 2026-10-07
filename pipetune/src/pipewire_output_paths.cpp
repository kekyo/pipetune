/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_paths.h"
#include "pipewire_graph_clock.h"

#include <pipewire/pipewire.h>
#include <spa/param/latency-utils.h>

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstring>
#include <map>
#include <string_view>

namespace pipetune {

using PathProperties = std::map<std::string, std::string, std::less<>>;

struct PathBinding {
  PipeWireOutputPaths *owner = nullptr;
  pw_proxy *proxy = nullptr;
  spa_hook listener = {};

  ~PathBinding() {
    if (proxy != nullptr) {
      spa_hook_remove(&listener);
      pw_proxy_destroy(proxy);
    }
  }
};

struct PathNode {
  PathBinding binding;
  PathProperties properties;
  pw_node_state state = PW_NODE_STATE_CREATING;
};

struct PathPort {
  PathBinding binding;
  std::uint32_t nodeId = PW_ID_ANY;
  pw_direction direction = PW_DIRECTION_INPUT;
  bool canEnumerate = false;
  bool control = false;
  bool monitor = false;
  std::optional<spa_latency_info> latency;
};

struct PathLink {
  PathBinding binding;
  std::uint32_t outputNodeId = PW_ID_ANY;
  std::uint32_t outputPortId = PW_ID_ANY;
  std::uint32_t inputNodeId = PW_ID_ANY;
  pw_link_state state = PW_LINK_STATE_INIT;
};

struct PipeWireOutputPaths {
  pw_core *core = nullptr;
  pw_registry *registry = nullptr;
  spa_hook registryListener = {};
  spa_hook coreListener = {};
  std::string publicInputName;
  PipeWireOutputPathsCallback callback = nullptr;
  void *userData = nullptr;
  std::map<std::uint32_t, std::unique_ptr<PathNode>> nodes;
  std::map<std::uint32_t, std::unique_ptr<PathPort>> ports;
  std::map<std::uint32_t, std::unique_ptr<PathLink>> links;
  PipeWireGraphClockPtr clockObserver;
  std::vector<PipeWireNodeClock> clocks;
  std::vector<PipeWireOutputPath> published;
  int pending = 0;
  bool initial = true;
};

static void updateProperties(PathProperties &properties, const spa_dict *dictionary) {
  if (dictionary == nullptr) return;
  for (auto index = std::uint32_t{0}; index < dictionary->n_items; ++index) {
    const auto &item = dictionary->items[index];
    if (item.value == nullptr) properties.erase(item.key);
    else properties[item.key] = item.value;
  }
}

static std::string_view property(const PathProperties &properties, std::string_view key) {
  const auto found = properties.find(key);
  return found == properties.end() ? std::string_view{} : std::string_view(found->second);
}

static std::uint64_t unsignedValue(std::string_view text) {
  if (text.empty()) return 0;
  auto value = std::uint64_t{};
  const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
  return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : 0;
}

static void scheduleSnapshot(PipeWireOutputPaths &observer) {
  observer.pending = pw_core_sync(observer.core, PW_ID_CORE, 0);
}

static PipeWireOutputPath describePath(const PipeWireOutputPaths &observer,
                                     std::uint32_t id, const PathNode &node) {
  auto path = PipeWireOutputPath{};
  path.outputId = property(node.properties, "node.pipetune.output-id");
  path.streamSerial = unsignedValue(property(node.properties, PW_KEY_OBJECT_SERIAL));
  path.targetSerial = unsignedValue(property(node.properties, "node.pipetune.target-serial"));
  const auto channels = unsignedValue(property(node.properties, "node.pipetune.output-channels"));
  if (path.streamSerial == 0 || path.targetSerial == 0 || channels == 0 || channels > 16) return path;
  const auto target = std::find_if(observer.nodes.begin(), observer.nodes.end(), [&](const auto &item) {
    return unsignedValue(property(item.second->properties, PW_KEY_OBJECT_SERIAL)) == path.targetSerial;
  });
  if (target == observer.nodes.end()) return path;
  path.targetNodeId = target->first;
  if (node.state == PW_NODE_STATE_ERROR || target->second->state == PW_NODE_STATE_ERROR) {
    path.activity = OutputPathActivity::error;
    return path;
  }
  if (node.state == PW_NODE_STATE_CREATING || target->second->state == PW_NODE_STATE_CREATING) return path;
  auto complete = true;
  auto active = node.state == PW_NODE_STATE_RUNNING && target->second->state == PW_NODE_STATE_RUNNING;
  auto count = std::size_t{0};
  auto latencyKnown = true;
  auto latency = spa_latency_info{};
  spa_latency_info_combine_start(&latency, SPA_DIRECTION_INPUT);
  for (const auto &[portId, port] : observer.ports) {
    if (port->nodeId != id || port->direction != PW_DIRECTION_OUTPUT || port->control || port->monitor) continue;
    ++count;
    auto links = std::size_t{0};
    for (const auto &[linkId, link] : observer.links) {
      if (link->outputNodeId != id || link->outputPortId != portId) continue;
      ++links;
      if (link->state == PW_LINK_STATE_ERROR || link->inputNodeId != path.targetNodeId) {
        path.activity = OutputPathActivity::error;
        return path;
      }
      complete = complete && link->state >= PW_LINK_STATE_PAUSED;
      active = active && link->state == PW_LINK_STATE_ACTIVE;
    }
    complete = complete && links == 1;
    latencyKnown = latencyKnown && port->latency.has_value();
    if (port->latency) spa_latency_info_combine(&latency, &*port->latency);
  }
  if (!complete || count != channels) return path;
  path.activity = active ? OutputPathActivity::active : OutputPathActivity::idle;
  if (!active || !latencyKnown) return path;
  const auto clock = std::find_if(observer.clocks.begin(), observer.clocks.end(), [&](const auto &item) {
    return item.nodeId == id && item.nodeSerial == path.streamSerial;
  });
  if (clock == observer.clocks.end()) return path;
  spa_latency_info_combine_finish(&latency);
  // Downstream reports are intervals. Their midpoint is only an estimate of
  // the delay seen by combine-stream, never a readout of its private buffers.
  const auto frames = (static_cast<double>(latency.min_quantum) + latency.max_quantum) * 0.5 * clock->quantum +
      (static_cast<double>(latency.min_rate) + latency.max_rate) * 0.5;
  const auto nanoseconds = frames * 1000000000.0 * clock->rateNumerator / clock->rateDenominator +
      (static_cast<double>(latency.min_ns) + static_cast<double>(latency.max_ns)) * 0.5;
  if (std::isfinite(nanoseconds) && nanoseconds >= 0) path.reportedLatencyNanoseconds = nanoseconds;
  return path;
}

static void publishSnapshot(PipeWireOutputPaths &observer) {
  auto paths = std::vector<PipeWireOutputPath>{};
  for (const auto &[id, node] : observer.nodes) {
    if (property(node->properties, "node.pipetune.public-target") != observer.publicInputName ||
        property(node->properties, "node.pipetune.managed-output") != "true" ||
        property(node->properties, "node.pipetune.output-id").empty()) continue;
    paths.push_back(describePath(observer, id, *node));
  }
  if (!observer.initial && paths == observer.published) return;
  observer.initial = false;
  observer.published = std::move(paths);
  observer.callback(observer.published, observer.userData);
}

static void nodeInfo(void *data, const pw_node_info *info) {
  auto &node = *static_cast<PathNode *>(data);
  if ((info->change_mask & PW_NODE_CHANGE_MASK_PROPS) != 0) updateProperties(node.properties, info->props);
  if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0) node.state = info->state;
  scheduleSnapshot(*node.binding.owner);
}

static void portInfo(void *data, const pw_port_info *info) {
  auto &port = *static_cast<PathPort *>(data);
  port.direction = info->direction;
  if ((info->change_mask & PW_PORT_CHANGE_MASK_PROPS) != 0 && info->props != nullptr) {
    if (const auto *value = spa_dict_lookup(info->props, PW_KEY_PORT_CONTROL)) port.control = std::strcmp(value, "true") == 0;
    if (const auto *value = spa_dict_lookup(info->props, PW_KEY_PORT_MONITOR)) port.monitor = std::strcmp(value, "true") == 0;
  }
  if ((info->change_mask & PW_PORT_CHANGE_MASK_PARAMS) != 0) {
    port.latency.reset();
    for (auto index = std::uint32_t{0}; port.canEnumerate && index < info->n_params; ++index) {
      const auto &parameter = info->params[index];
      if (parameter.id == SPA_PARAM_Latency && (parameter.flags & SPA_PARAM_INFO_READ) != 0)
        pw_port_enum_params(reinterpret_cast<pw_port *>(port.binding.proxy), 0, SPA_PARAM_Latency, 0, UINT32_MAX, nullptr);
    }
  }
  scheduleSnapshot(*port.binding.owner);
}

static void portParam(void *data, int, std::uint32_t id, std::uint32_t, std::uint32_t, const spa_pod *param) {
  if (id != SPA_PARAM_Latency) return;
  auto &port = *static_cast<PathPort *>(data);
  auto latency = spa_latency_info{};
  if (param != nullptr && spa_latency_parse(param, &latency) >= 0 && latency.direction == SPA_DIRECTION_INPUT) {
    // SPA stores these signed wire integers in unsigned members. Reject their
    // negative encodings, inverted ranges, and non-finite quantum values.
    port.latency.reset();
    if (std::isfinite(latency.min_quantum) && std::isfinite(latency.max_quantum) &&
        latency.min_quantum >= 0 && latency.min_quantum <= latency.max_quantum &&
        latency.min_rate <= latency.max_rate && latency.max_rate <= INT32_MAX &&
        latency.min_ns <= latency.max_ns && latency.max_ns <= INT64_MAX) port.latency = latency;
  } else if (param == nullptr) port.latency.reset();
  scheduleSnapshot(*port.binding.owner);
}

static void linkInfo(void *data, const pw_link_info *info) {
  auto &link = *static_cast<PathLink *>(data);
  link.outputNodeId = info->output_node_id;
  link.outputPortId = info->output_port_id;
  link.inputNodeId = info->input_node_id;
  if ((info->change_mask & PW_LINK_CHANGE_MASK_STATE) != 0) link.state = info->state;
  scheduleSnapshot(*link.binding.owner);
}

static void globalAdded(void *data, std::uint32_t id, std::uint32_t permissions,
                        const char *type, std::uint32_t version, const spa_dict *props) {
  if ((permissions & PW_PERM_R) == 0) return;
  auto &observer = *static_cast<PipeWireOutputPaths *>(data);
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
    auto node = std::make_unique<PathNode>();
    node->binding.owner = &observer;
    updateProperties(node->properties, props);
    node->binding.proxy = static_cast<pw_proxy *>(pw_registry_bind(observer.registry, id, type,
        std::min<std::uint32_t>(version, PW_VERSION_NODE), 0));
    if (node->binding.proxy == nullptr) return;
    static const auto events = pw_node_events{.version = PW_VERSION_NODE_EVENTS, .info = nodeInfo, .param = nullptr};
    pw_node_add_listener(reinterpret_cast<pw_node *>(node->binding.proxy), &node->binding.listener, &events, node.get());
    observer.nodes[id] = std::move(node);
  } else if (std::strcmp(type, PW_TYPE_INTERFACE_Port) == 0 && props != nullptr) {
    const auto *nodeId = spa_dict_lookup(props, PW_KEY_NODE_ID);
    if (nodeId == nullptr) return;
    const auto parent = unsignedValue(nodeId);
    if (parent == 0 || parent >= PW_ID_ANY) return;
    auto port = std::make_unique<PathPort>();
    port->binding.owner = &observer;
    port->nodeId = static_cast<std::uint32_t>(parent);
    port->canEnumerate = (permissions & PW_PERM_X) != 0;
    port->binding.proxy = static_cast<pw_proxy *>(pw_registry_bind(observer.registry, id, type,
        std::min<std::uint32_t>(version, PW_VERSION_PORT), 0));
    if (port->binding.proxy == nullptr) return;
    static const auto events = pw_port_events{.version = PW_VERSION_PORT_EVENTS, .info = portInfo, .param = portParam};
    pw_port_add_listener(reinterpret_cast<pw_port *>(port->binding.proxy), &port->binding.listener, &events, port.get());
    observer.ports[id] = std::move(port);
  } else if (std::strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
    auto link = std::make_unique<PathLink>();
    link->binding.owner = &observer;
    link->binding.proxy = static_cast<pw_proxy *>(pw_registry_bind(observer.registry, id, type,
        std::min<std::uint32_t>(version, PW_VERSION_LINK), 0));
    if (link->binding.proxy == nullptr) return;
    static const auto events = pw_link_events{.version = PW_VERSION_LINK_EVENTS, .info = linkInfo};
    pw_link_add_listener(reinterpret_cast<pw_link *>(link->binding.proxy), &link->binding.listener, &events, link.get());
    observer.links[id] = std::move(link);
  } else return;
  scheduleSnapshot(observer);
}

static void globalRemoved(void *data, std::uint32_t id) {
  auto &observer = *static_cast<PipeWireOutputPaths *>(data);
  observer.nodes.erase(id);
  observer.ports.erase(id);
  observer.links.erase(id);
  scheduleSnapshot(observer);
}

PipeWireOutputPathsPtr observePipeWireOutputPaths(pw_core *core, const std::string &publicInputName,
    PipeWireOutputPathsCallback callback, void *userData) {
  if (core == nullptr || publicInputName.empty() || callback == nullptr) return {};
  auto observer = PipeWireOutputPathsPtr(new PipeWireOutputPaths{});
  observer->core = core;
  observer->publicInputName = publicInputName;
  observer->callback = callback;
  observer->userData = userData;
  observer->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (observer->registry == nullptr) return {};
  static const auto events = pw_registry_events{
      .version = PW_VERSION_REGISTRY_EVENTS, .global = globalAdded, .global_remove = globalRemoved};
  static const auto coreEvents = [] {
    auto value = pw_core_events{};
    value.version = PW_VERSION_CORE_EVENTS;
    value.done = [](void *data, std::uint32_t id, int sequence) {
      auto &observer = *static_cast<PipeWireOutputPaths *>(data);
      if (id == PW_ID_CORE && sequence == observer.pending) publishSnapshot(observer);
    };
    value.error = [](void *data, std::uint32_t id, int, int code, const char *) {
      if (id != PW_ID_CORE || code != -EPIPE) return;
      auto &observer = *static_cast<PipeWireOutputPaths *>(data);
      observer.nodes.clear();
      publishSnapshot(observer);
    };
    return value;
  }();
  pw_core_add_listener(core, &observer->coreListener, &coreEvents, observer.get());
  pw_registry_add_listener(observer->registry, &observer->registryListener, &events, observer.get());
  observer->clockObserver = observePipeWireGraphClocks(core, [](const auto &clocks, void *data) {
    auto &observer = *static_cast<PipeWireOutputPaths *>(data);
    observer.clocks = clocks;
    scheduleSnapshot(observer);
  }, observer.get());
  scheduleSnapshot(*observer);
  return observer;
}

void PipeWireOutputPathsDeleter::operator()(PipeWireOutputPaths *observer) const noexcept {
  if (observer == nullptr) return;
  observer->clockObserver.reset();
  observer->links.clear();
  observer->ports.clear();
  observer->nodes.clear();
  if (observer->registry != nullptr) {
    spa_hook_remove(&observer->registryListener);
    spa_hook_remove(&observer->coreListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(observer->registry));
  }
  delete observer;
}

} // namespace pipetune
