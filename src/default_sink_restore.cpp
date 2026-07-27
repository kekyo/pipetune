#include "default_sink_restore.h"

#include "output_device_tracker.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <yyjson.h>

#include <algorithm>
#include <cerrno>
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune {

constexpr auto kRestoreTimeoutSeconds = std::time_t{5};

enum class RestorePhase {
  enumerating,
  writing
};

struct DefaultSinkRestoreRuntime {
  OutputDeviceTracker tracker;
  pw_main_loop *mainLoop;
  pw_context *context;
  pw_core *core;
  pw_registry *registry;
  pw_metadata *metadata;
  spa_source *timeoutSource;
  pw_core_events coreEvents;
  pw_registry_events registryEvents;
  pw_metadata_events metadataEvents;
  spa_hook coreListener;
  spa_hook registryListener;
  spa_hook metadataListener;
  bool coreListenerAdded;
  bool registryListenerAdded;
  bool metadataListenerAdded;
  std::uint32_t metadataId;
  int pendingSequence;
  RestorePhase phase;
  bool completed;
  std::string selectedTarget;
  std::string metadataValue;
  std::string error;

  explicit DefaultSinkRestoreRuntime(std::string excludedNodeName)
      : tracker(std::move(excludedNodeName), ""), mainLoop(nullptr),
        context(nullptr), core(nullptr), registry(nullptr), metadata(nullptr),
        timeoutSource(nullptr), coreEvents{}, registryEvents{},
        metadataEvents{}, coreListener{}, registryListener{},
        metadataListener{}, coreListenerAdded(false),
        registryListenerAdded(false), metadataListenerAdded(false),
        metadataId(PW_ID_ANY), pendingSequence(0),
        phase(RestorePhase::enumerating), completed(false), selectedTarget(),
        metadataValue(), error() {}

  ~DefaultSinkRestoreRuntime() {
    if (mainLoop != nullptr && timeoutSource != nullptr) {
      pw_loop_destroy_source(pw_main_loop_get_loop(mainLoop), timeoutSource);
    }
    if (metadata != nullptr) {
      if (metadataListenerAdded) {
        spa_hook_remove(&metadataListener);
      }
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata));
    }
    if (registry != nullptr) {
      if (registryListenerAdded) {
        spa_hook_remove(&registryListener);
      }
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    }
    if (core != nullptr) {
      if (coreListenerAdded) {
        spa_hook_remove(&coreListener);
      }
      pw_core_disconnect(core);
    }
    if (context != nullptr) {
      pw_context_destroy(context);
    }
    if (mainLoop != nullptr) {
      pw_main_loop_destroy(mainLoop);
    }
  }
};

struct RestorePipeWireLibraryScope {
  RestorePipeWireLibraryScope() {
    pw_init(nullptr, nullptr);
  }

  ~RestorePipeWireLibraryScope() {
    pw_deinit();
  }
};

struct JsonDocumentDeleterForMetadata {
  void operator()(yyjson_doc *document) const noexcept {
    yyjson_doc_free(document);
  }
};

struct MutableJsonDocumentDeleterForMetadata {
  void operator()(yyjson_mut_doc *document) const noexcept {
    yyjson_mut_doc_free(document);
  }
};

static std::string restoreSystemError(std::string_view operation, int result) {
  const auto errorNumber = result < 0 ? -result : errno;
  return std::string(operation) + ": " + std::strerror(errorNumber);
}

static void failRestore(DefaultSinkRestoreRuntime &runtime,
                        std::string message) {
  if (!runtime.error.empty()) {
    return;
  }
  runtime.error = std::move(message);
  if (runtime.mainLoop != nullptr) {
    pw_main_loop_quit(runtime.mainLoop);
  }
}

std::string makeDefaultSinkMetadataValue(std::string_view nodeName) {
  using MutableDocument =
      std::unique_ptr<yyjson_mut_doc,
                      MutableJsonDocumentDeleterForMetadata>;
  auto document = MutableDocument(yyjson_mut_doc_new(nullptr));
  if (document == nullptr) {
    return {};
  }
  auto *root = yyjson_mut_obj(document.get());
  if (root == nullptr ||
      !yyjson_mut_obj_add_strncpy(document.get(), root, "name",
                                  nodeName.data(), nodeName.size())) {
    return {};
  }
  yyjson_mut_doc_set_root(document.get(), root);
  auto length = std::size_t{0};
  auto *encoded = yyjson_mut_write(
      document.get(),
      YYJSON_WRITE_ESCAPE_UNICODE | YYJSON_WRITE_ALLOW_INVALID_UNICODE,
      &length);
  if (encoded == nullptr) {
    return {};
  }
  auto result = std::string(encoded, length);
  std::free(encoded);
  return result;
}

std::string defaultSinkNameFromMetadata(const char *value) {
  if (value == nullptr) {
    return {};
  }
  using Document =
      std::unique_ptr<yyjson_doc, JsonDocumentDeleterForMetadata>;
  auto document =
      Document(yyjson_read(value, std::strlen(value), YYJSON_READ_NOFLAG));
  if (document == nullptr) {
    return {};
  }
  auto *root = yyjson_doc_get_root(document.get());
  auto *name = yyjson_is_obj(root) ? yyjson_obj_get(root, "name") : nullptr;
  return yyjson_is_str(name)
             ? std::string(yyjson_get_str(name), yyjson_get_len(name))
             : std::string{};
}

static std::int32_t restorePriority(const char *value) noexcept {
  if (value == nullptr) {
    return 0;
  }
  auto priority = std::int32_t{0};
  const auto *end = value + std::strlen(value);
  const auto result = std::from_chars(value, end, priority);
  return result.ec == std::errc{} && result.ptr == end ? priority : 0;
}

static bool restoreBoolean(const char *value) noexcept {
  return value != nullptr &&
         (std::string_view(value) == "true" || std::string_view(value) == "1");
}

static void requestRestoreSync(DefaultSinkRestoreRuntime &runtime) {
  const auto sequence = pw_core_sync(runtime.core, PW_ID_CORE, 0);
  if (sequence < 0) {
    failRestore(runtime,
                restoreSystemError("cannot synchronize PipeWire", sequence));
    return;
  }
  runtime.pendingSequence = sequence;
}

static void restoreCoreDone(void *data, std::uint32_t id, int sequence) {
  auto &runtime = *static_cast<DefaultSinkRestoreRuntime *>(data);
  if (id != PW_ID_CORE || sequence != runtime.pendingSequence) {
    return;
  }
  if (runtime.phase == RestorePhase::writing) {
    runtime.completed = true;
    pw_main_loop_quit(runtime.mainLoop);
    return;
  }

  runtime.tracker.commitSelection();
  runtime.selectedTarget = runtime.tracker.selectedTarget();
  if (runtime.metadata == nullptr) {
    failRestore(runtime, "PipeWire default metadata is unavailable");
    return;
  }
  if (runtime.selectedTarget.empty()) {
    failRestore(runtime, "no physical PipeWire output sink is available");
    return;
  }
  runtime.metadataValue =
      makeDefaultSinkMetadataValue(runtime.selectedTarget);
  if (runtime.metadataValue.empty()) {
    failRestore(runtime, "cannot encode physical PipeWire default sink");
    return;
  }
  const auto result = pw_metadata_set_property(
      runtime.metadata, PW_ID_CORE, "default.audio.sink", "Spa:String:JSON",
      runtime.metadataValue.c_str());
  if (result < 0) {
    failRestore(runtime,
                restoreSystemError("cannot restore PipeWire default sink",
                                   result));
    return;
  }
  runtime.phase = RestorePhase::writing;
  requestRestoreSync(runtime);
}

static void restoreCoreError(void *data, std::uint32_t, int, int result,
                             const char *message) {
  auto &runtime = *static_cast<DefaultSinkRestoreRuntime *>(data);
  const auto detail =
      message == nullptr
          ? restoreSystemError("PipeWire core error", result)
          : std::string(message);
  failRestore(runtime, "PipeWire default restoration failed: " + detail);
}

static int restoreMetadataProperty(void *data, std::uint32_t subject,
                                   const char *key, const char *,
                                   const char *value) {
  auto &runtime = *static_cast<DefaultSinkRestoreRuntime *>(data);
  if (subject == PW_ID_CORE && key != nullptr &&
      std::string_view(key) == "default.audio.sink") {
    runtime.tracker.setDefaultTarget(defaultSinkNameFromMetadata(value));
  }
  return 0;
}

static void restoreRegistryGlobal(void *data, std::uint32_t id,
                                  std::uint32_t, const char *type,
                                  std::uint32_t version,
                                  const spa_dict *properties) {
  auto &runtime = *static_cast<DefaultSinkRestoreRuntime *>(data);
  if (type == nullptr || properties == nullptr) {
    return;
  }
  if (std::string_view(type) == PW_TYPE_INTERFACE_Node) {
    const auto *mediaClass = spa_dict_lookup(properties, PW_KEY_MEDIA_CLASS);
    const auto *name = spa_dict_lookup(properties, PW_KEY_NODE_NAME);
    if (mediaClass == nullptr || std::string_view(mediaClass) != "Audio/Sink" ||
        name == nullptr) {
      return;
    }
    const auto *serial = spa_dict_lookup(properties, PW_KEY_OBJECT_SERIAL);
    const auto *priority =
        spa_dict_lookup(properties, PW_KEY_PRIORITY_SESSION);
    const auto *virtualNode =
        spa_dict_lookup(properties, PW_KEY_NODE_VIRTUAL);
    runtime.tracker.updateDevice(
        {.id = id,
         .name = name,
         .objectSerial =
             serial == nullptr ? std::string{} : std::string(serial),
         .priority = restorePriority(priority),
         .virtualNode = restoreBoolean(virtualNode)});
    return;
  }
  if (std::string_view(type) != PW_TYPE_INTERFACE_Metadata ||
      runtime.metadata != nullptr) {
    return;
  }
  const auto *metadataName =
      spa_dict_lookup(properties, PW_KEY_METADATA_NAME);
  if (metadataName == nullptr || std::string_view(metadataName) != "default") {
    return;
  }

  runtime.metadata = static_cast<pw_metadata *>(pw_registry_bind(
      runtime.registry, id, type,
      std::min(version, static_cast<std::uint32_t>(PW_VERSION_METADATA)), 0));
  if (runtime.metadata == nullptr) {
    failRestore(runtime,
                restoreSystemError("cannot bind PipeWire default metadata",
                                   -errno));
    return;
  }
  runtime.metadataId = id;
  runtime.metadataEvents.version = PW_VERSION_METADATA_EVENTS;
  runtime.metadataEvents.property = restoreMetadataProperty;
  const auto listenerResult =
      pw_metadata_add_listener(runtime.metadata, &runtime.metadataListener,
                               &runtime.metadataEvents, &runtime);
  if (listenerResult < 0) {
    failRestore(runtime,
                restoreSystemError("cannot monitor PipeWire default metadata",
                                   listenerResult));
    return;
  }
  runtime.metadataListenerAdded = true;
  requestRestoreSync(runtime);
}

static void restoreRegistryGlobalRemoved(void *data, std::uint32_t id) {
  auto &runtime = *static_cast<DefaultSinkRestoreRuntime *>(data);
  runtime.tracker.removeDevice(id);
  if (id != runtime.metadataId || runtime.metadata == nullptr) {
    return;
  }
  if (runtime.metadataListenerAdded) {
    spa_hook_remove(&runtime.metadataListener);
    runtime.metadataListenerAdded = false;
  }
  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.metadata));
  runtime.metadata = nullptr;
  runtime.metadataId = PW_ID_ANY;
}

static void restoreTimedOut(void *data, std::uint64_t) {
  auto &runtime = *static_cast<DefaultSinkRestoreRuntime *>(data);
  failRestore(runtime, "timed out while restoring the PipeWire default sink");
}

static bool prepareRestoreRuntime(DefaultSinkRestoreRuntime &runtime) {
  runtime.mainLoop = pw_main_loop_new(nullptr);
  if (runtime.mainLoop == nullptr) {
    failRestore(runtime,
                restoreSystemError("cannot create PipeWire main loop", -errno));
    return false;
  }
  runtime.timeoutSource = pw_loop_add_timer(
      pw_main_loop_get_loop(runtime.mainLoop), restoreTimedOut, &runtime);
  if (runtime.timeoutSource == nullptr) {
    failRestore(runtime,
                restoreSystemError("cannot create restoration timer", -errno));
    return false;
  }
  auto delay =
      timespec{.tv_sec = kRestoreTimeoutSeconds, .tv_nsec = 0};
  auto interval = timespec{.tv_sec = 0, .tv_nsec = 0};
  const auto timerResult = pw_loop_update_timer(
      pw_main_loop_get_loop(runtime.mainLoop), runtime.timeoutSource, &delay,
      &interval, false);
  if (timerResult < 0) {
    failRestore(runtime,
                restoreSystemError("cannot arm restoration timer",
                                   timerResult));
    return false;
  }

  runtime.context =
      pw_context_new(pw_main_loop_get_loop(runtime.mainLoop), nullptr, 0);
  if (runtime.context == nullptr) {
    failRestore(runtime,
                restoreSystemError("cannot create PipeWire context", -errno));
    return false;
  }
  runtime.core = pw_context_connect(runtime.context, nullptr, 0);
  if (runtime.core == nullptr) {
    failRestore(runtime,
                restoreSystemError("cannot connect to PipeWire", -errno));
    return false;
  }
  runtime.coreEvents.version = PW_VERSION_CORE_EVENTS;
  runtime.coreEvents.done = restoreCoreDone;
  runtime.coreEvents.error = restoreCoreError;
  const auto coreListenerResult =
      pw_core_add_listener(runtime.core, &runtime.coreListener,
                           &runtime.coreEvents, &runtime);
  if (coreListenerResult < 0) {
    failRestore(runtime,
                restoreSystemError("cannot monitor PipeWire core",
                                   coreListenerResult));
    return false;
  }
  runtime.coreListenerAdded = true;

  runtime.registry =
      pw_core_get_registry(runtime.core, PW_VERSION_REGISTRY, 0);
  if (runtime.registry == nullptr) {
    failRestore(runtime,
                restoreSystemError("cannot access PipeWire registry", -errno));
    return false;
  }
  runtime.registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
  runtime.registryEvents.global = restoreRegistryGlobal;
  runtime.registryEvents.global_remove = restoreRegistryGlobalRemoved;
  const auto registryListenerResult =
      pw_registry_add_listener(runtime.registry, &runtime.registryListener,
                               &runtime.registryEvents, &runtime);
  if (registryListenerResult < 0) {
    failRestore(runtime,
                restoreSystemError("cannot monitor PipeWire registry",
                                   registryListenerResult));
    return false;
  }
  runtime.registryListenerAdded = true;
  requestRestoreSync(runtime);
  return runtime.error.empty();
}

DefaultSinkRestoreResult
restorePipeWireDefaultSink(std::string excludedNodeName) {
  auto library = RestorePipeWireLibraryScope{};
  auto runtime =
      DefaultSinkRestoreRuntime(std::move(excludedNodeName));
  if (prepareRestoreRuntime(runtime)) {
    const auto runResult = pw_main_loop_run(runtime.mainLoop);
    if (runResult < 0 && runtime.error.empty()) {
      failRestore(runtime,
                  restoreSystemError("PipeWire main loop failed", runResult));
    } else if (!runtime.completed && runtime.error.empty()) {
      failRestore(runtime,
                  "PipeWire main loop stopped before default restoration");
    }
  }
  return {.success = runtime.completed && runtime.error.empty(),
          .selectedTarget = runtime.selectedTarget,
          .error = runtime.error};
}

} // namespace pipetune
