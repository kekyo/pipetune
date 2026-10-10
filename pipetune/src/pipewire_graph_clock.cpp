/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_graph_clock.h"

#include <pipewire/pipewire.h>
#include <pipewire/extensions/profiler.h>
#include <pipewire/impl-module.h>
#include <spa/param/profiler.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <map>
#include <string_view>

namespace pipetune {

struct ClockNode {
  PipeWireGraphClock *owner;
  pw_node *proxy = nullptr;
  spa_hook listener = {};
  PipeWireNodeClock description;
  pw_node_state state = PW_NODE_STATE_CREATING;
  std::int64_t earliest = 0;
  std::int64_t latest = 0;
  bool measured = false;

  ~ClockNode() {
    if (proxy != nullptr) {
      spa_hook_remove(&listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
    }
  }
};

struct PipeWireGraphClock {
  pw_registry *registry = nullptr;
  pw_profiler *profiler = nullptr;
  std::uint32_t profilerId = PW_ID_ANY;
  spa_hook registryListener = {};
  spa_hook coreListener = {};
  spa_hook profilerListener = {};
  std::map<std::uint32_t, std::unique_ptr<ClockNode>> nodes;
  PipeWireGraphClockCallback callback = nullptr;
  void *userData = nullptr;
  std::vector<PipeWireNodeClock> published;
  bool initial = true;
};

static std::int64_t monotonicNanoseconds() noexcept {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void publishClocks(PipeWireGraphClock &observer) {
  auto clocks = std::vector<PipeWireNodeClock>{};
  for (const auto &[id, node] : observer.nodes) {
    if (node->measured) clocks.push_back(node->description);
  }
  if (!observer.initial && clocks == observer.published) return;
  observer.initial = false;
  observer.published = std::move(clocks);
  observer.callback(observer.published, observer.userData);
}

static void nodeInfo(void *data, const pw_node_info *info) {
  auto &node = *static_cast<ClockNode *>(data);
  if ((info->change_mask & PW_NODE_CHANGE_MASK_STATE) != 0 && info->state != node.state) {
    node.state = info->state;
    node.measured = false;
    // Profiler messages can be queued across a stop/restart or reused node ID.
    // Accept only measurements taken after this node became runnable.
    node.earliest = monotonicNanoseconds();
    node.latest = 0;
    publishClocks(*node.owner);
  }
}

static void profile(void *data, const spa_pod *pod) {
  if (pod == nullptr || !spa_pod_is_struct(pod)) return;
  auto &observer = *static_cast<PipeWireGraphClock *>(data);
  auto changed = false;
  for (auto *entry = static_cast<const spa_pod *>(SPA_POD_BODY(pod));
       spa_pod_is_inside(SPA_POD_BODY(pod), SPA_POD_BODY_SIZE(pod), entry);
       entry = static_cast<const spa_pod *>(spa_pod_next(entry))) {
    if (!spa_pod_is_object_type(entry, SPA_TYPE_OBJECT_Profiler)) continue;
    const auto *clock = spa_pod_find_prop(entry, nullptr, SPA_PROFILER_clock);
    if (clock == nullptr) continue;
    auto flags = std::int32_t{};
    auto clockId = std::int32_t{};
    const char *name = nullptr;
    auto timestamp = std::int64_t{};
    auto position = std::int64_t{};
    auto quantum = std::int64_t{};
    auto rate = spa_fraction{};
    if (spa_pod_parse_struct(&clock->value, SPA_POD_Int(&flags), SPA_POD_Int(&clockId),
        SPA_POD_String(&name), SPA_POD_Long(&timestamp), SPA_POD_Fraction(&rate),
        SPA_POD_Long(&position), SPA_POD_Long(&quantum)) < 0 ||
        timestamp <= 0 || quantum <= 0 || rate.num == 0 || rate.denom == 0) continue;
    // Each profile carries its own driver clock and follower IDs. Never use
    // another driver's rate, a node's requested rate, or the DSP sample rate.
    const spa_pod_prop *property;
    SPA_POD_OBJECT_FOREACH(reinterpret_cast<const spa_pod_object *>(entry), property) {
      if (property->key != SPA_PROFILER_driverBlock && property->key != SPA_PROFILER_followerBlock) continue;
      auto id = std::int32_t{-1};
      if (spa_pod_parse_struct(&property->value, SPA_POD_Int(&id)) < 0 || id < 0) continue;
      const auto found = observer.nodes.find(static_cast<std::uint32_t>(id));
      if (found == observer.nodes.end()) continue;
      auto &node = *found->second;
      if (node.state != PW_NODE_STATE_RUNNING || timestamp < node.earliest || timestamp < node.latest) continue;
      node.latest = timestamp;
      changed = changed || !node.measured || node.description.rateNumerator != rate.num ||
          node.description.rateDenominator != rate.denom ||
          node.description.quantum != static_cast<std::uint64_t>(quantum);
      node.description.rateNumerator = rate.num;
      node.description.rateDenominator = rate.denom;
      node.description.quantum = static_cast<std::uint64_t>(quantum);
      node.measured = true;
    }
  }
  if (changed) publishClocks(observer);
}

static void globalAdded(void *data, std::uint32_t id, std::uint32_t permissions,
                        const char *type, std::uint32_t version, const spa_dict *props) {
  if ((permissions & PW_PERM_R) == 0) return;
  auto &observer = *static_cast<PipeWireGraphClock *>(data);
  if (std::strcmp(type, PW_TYPE_INTERFACE_Profiler) == 0 && observer.profiler == nullptr) {
    observer.profiler = static_cast<pw_profiler *>(pw_registry_bind(observer.registry, id, type,
        std::min<std::uint32_t>(version, PW_VERSION_PROFILER), 0));
    if (observer.profiler == nullptr) return;
    observer.profilerId = id;
    static const auto events = pw_profiler_events{.version = PW_VERSION_PROFILER_EVENTS, .profile = profile};
    pw_proxy_add_object_listener(reinterpret_cast<pw_proxy *>(observer.profiler),
        &observer.profilerListener, &events, &observer);
    return;
  }
  if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || props == nullptr) return;
  const auto *serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
  const auto *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  if (serial == nullptr || name == nullptr) return;
  auto node = std::make_unique<ClockNode>();
  const auto serialText = std::string_view(serial);
  const auto parsed = std::from_chars(serialText.data(), serialText.data() + serialText.size(), node->description.nodeSerial);
  if (parsed.ec != std::errc{} || parsed.ptr != serialText.data() + serialText.size() || node->description.nodeSerial == 0) return;
  node->owner = &observer;
  node->earliest = monotonicNanoseconds();
  node->description.nodeId = id;
  node->description.nodeName = name;
  node->proxy = static_cast<pw_node *>(pw_registry_bind(observer.registry, id, type,
      std::min<std::uint32_t>(version, PW_VERSION_NODE), 0));
  if (node->proxy == nullptr) return;
  static const auto events = pw_node_events{.version = PW_VERSION_NODE_EVENTS, .info = nodeInfo, .param = nullptr};
  pw_node_add_listener(node->proxy, &node->listener, &events, node.get());
  observer.nodes[id] = std::move(node);
}

static void globalRemoved(void *data, std::uint32_t id) {
  auto &observer = *static_cast<PipeWireGraphClock *>(data);
  observer.nodes.erase(id);
  if (id == observer.profilerId) {
    spa_hook_remove(&observer.profilerListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(observer.profiler));
    observer.profiler = nullptr;
    observer.profilerId = PW_ID_ANY;
    for (auto &[nodeId, node] : observer.nodes) node->measured = false;
  }
  publishClocks(observer);
}

PipeWireGraphClockPtr observePipeWireGraphClocks(
    pw_core *core, PipeWireGraphClockCallback callback, void *userData) {
  if (core == nullptr || callback == nullptr) return {};
  auto observer = PipeWireGraphClockPtr(new PipeWireGraphClock{});
  observer->callback = callback;
  observer->userData = userData;
  // Protocol marshal tables belong to the context and point into this module.
  // Keep one context-owned module until context destruction, even while all
  // observers are stopped. Unloading it earlier leaves dangling marshal tables.
  auto *context = pw_core_get_context(core);
  constexpr auto profilerKey = "pipetune.profiler-protocol";
  if (pw_context_get_object(context, profilerKey) == nullptr) {
    auto *module = pw_context_load_module(context, PW_EXTENSION_MODULE_PROFILER, nullptr, nullptr);
    if (module == nullptr || pw_context_set_object(context, profilerKey, module) < 0) return {};
  }
  observer->registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  if (observer->registry == nullptr) return {};
  static const auto events = pw_registry_events{
      .version = PW_VERSION_REGISTRY_EVENTS, .global = globalAdded, .global_remove = globalRemoved};
  static const auto coreEvents = [] {
    auto value = pw_core_events{};
    value.version = PW_VERSION_CORE_EVENTS;
    value.done = [](void *data, std::uint32_t id, int) {
      if (id == PW_ID_CORE) publishClocks(*static_cast<PipeWireGraphClock *>(data));
    };
    value.error = [](void *data, std::uint32_t id, int, int code, const char *) {
      if (id != PW_ID_CORE || code != -EPIPE) return;
      auto &observer = *static_cast<PipeWireGraphClock *>(data);
      for (auto &[nodeId, node] : observer.nodes) node->measured = false;
      publishClocks(observer);
    };
    return value;
  }();
  pw_core_add_listener(core, &observer->coreListener, &coreEvents, observer.get());
  pw_registry_add_listener(observer->registry, &observer->registryListener, &events, observer.get());
  pw_core_sync(core, PW_ID_CORE, 0);
  return observer;
}

void PipeWireGraphClockDeleter::operator()(PipeWireGraphClock *observer) const noexcept {
  if (observer == nullptr) return;
  observer->nodes.clear();
  if (observer->profiler != nullptr) {
    spa_hook_remove(&observer->profilerListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(observer->profiler));
  }
  if (observer->registry != nullptr) {
    spa_hook_remove(&observer->registryListener);
    spa_hook_remove(&observer->coreListener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(observer->registry));
  }
  delete observer;
}

} // namespace pipetune
