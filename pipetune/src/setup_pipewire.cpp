#include "setup_pipewire.h"

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

#include <yyjson.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <time.h>
#include <utility>

namespace pipetune {

constexpr auto kSetupIntegrationTimeoutSeconds = time_t{10};
constexpr auto kPolicyMetadataName = std::string_view{"pipetune-policy"};
constexpr auto kDefaultMetadataName = std::string_view{"default"};
constexpr auto kLegacySinkName = std::string_view{"pipetune_sink"};
constexpr auto kConfiguredDefaultSinkKey =
    std::string_view{"default.configured.audio.sink"};
constexpr auto kEffectiveDefaultSinkKey =
    std::string_view{"default.audio.sink"};

struct SetupPipeWireRuntime {
  pw_main_loop *mainLoop;
  pw_context *context;
  pw_core *core;
  pw_registry *registry;
  pw_metadata *policyMetadata;
  pw_metadata *defaultMetadata;
  std::uint32_t policyMetadataId;
  std::uint32_t defaultMetadataId;
  pw_core_events coreEvents;
  pw_registry_events registryEvents;
  pw_metadata_events policyEvents;
  pw_metadata_events defaultEvents;
  spa_hook coreListener;
  spa_hook registryListener;
  spa_hook policyListener;
  spa_hook defaultListener;
  spa_source *timeoutSource;
  int pendingSequence;
  bool coreListenerInstalled;
  bool registryListenerInstalled;
  bool policyListenerInstalled;
  bool defaultListenerInstalled;
  bool registrySynchronized;
  bool legacyClearRequested;
  bool legacyDefaultCleared;
  bool completed;
  std::string policyProtocol;
  std::string policyBackend;
  std::string policyState;
  std::string error;
};

struct SetupPipeWireLibraryScope {
  SetupPipeWireLibraryScope() {
    pw_init(nullptr, nullptr);
  }

  ~SetupPipeWireLibraryScope() {
    pw_deinit();
  }
};

struct JsonDocumentDeleter {
  void operator()(yyjson_doc *document) const noexcept {
    yyjson_doc_free(document);
  }
};

using JsonDocument = std::unique_ptr<yyjson_doc, JsonDocumentDeleter>;
using SetupMetadataPropertyCallback = int (*)(
    void *, std::uint32_t, const char *, const char *, const char *);

static std::string setupPipeWireSystemError(std::string_view operation,
                                            int result) {
  const auto errorNumber = result < 0 ? -result : errno;
  return std::string(operation) + ": " + std::strerror(errorNumber);
}

static std::string setupDictionaryString(const spa_dict *dictionary,
                                         const char *key) {
  const auto *value =
      dictionary == nullptr ? nullptr : spa_dict_lookup(dictionary, key);
  return value == nullptr ? std::string{} : std::string(value);
}

static void failSetupPipeWireIntegration(SetupPipeWireRuntime &runtime,
                                         std::string error) {
  if (!runtime.error.empty() || runtime.completed) {
    return;
  }
  runtime.error = std::move(error);
  if (runtime.mainLoop != nullptr) {
    pw_main_loop_quit(runtime.mainLoop);
  }
}

static bool setupPolicyReady(const SetupPipeWireRuntime &runtime) noexcept {
  return runtime.policyProtocol == "1" && runtime.policyState == "ready" &&
         (runtime.policyBackend == "wireplumber-0.4" ||
          runtime.policyBackend == "wireplumber-0.5");
}

static bool requestSetupPipeWireSync(SetupPipeWireRuntime &runtime,
                                     std::string_view operation) {
  const auto sequence = pw_core_sync(runtime.core, PW_ID_CORE, 0);
  if (sequence < 0) {
    failSetupPipeWireIntegration(
        runtime, setupPipeWireSystemError(operation, sequence));
    return false;
  }
  runtime.pendingSequence = sequence;
  return true;
}

static bool metadataSelectsLegacySink(const char *value) {
  if (value == nullptr) {
    return false;
  }
  auto document = JsonDocument(
      yyjson_read(value, std::strlen(value), YYJSON_READ_NOFLAG));
  if (document == nullptr) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document.get());
  auto *name = yyjson_is_obj(root) ? yyjson_obj_get(root, "name") : nullptr;
  return yyjson_is_str(name) &&
         std::string_view(yyjson_get_str(name), yyjson_get_len(name)) ==
             kLegacySinkName;
}

static bool isLegacyDefaultKey(std::string_view key) noexcept {
  return key == kConfiguredDefaultSinkKey ||
         key == kEffectiveDefaultSinkKey;
}

static int setupPolicyProperty(void *data, std::uint32_t subject,
                               const char *key, const char *,
                               const char *value) {
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  if (subject != PW_ID_CORE || key == nullptr) {
    return 0;
  }
  auto *destination = static_cast<std::string *>(nullptr);
  if (std::string_view(key) == "protocol.version") {
    destination = &runtime.policyProtocol;
  } else if (std::string_view(key) == "policy.backend") {
    destination = &runtime.policyBackend;
  } else if (std::string_view(key) == "policy.state") {
    destination = &runtime.policyState;
  }
  if (destination != nullptr) {
    *destination = value == nullptr ? std::string{} : std::string(value);
    static_cast<void>(requestSetupPipeWireSync(
        runtime, "cannot synchronize WirePlumber policy handshake"));
  }
  return 0;
}

static int setupDefaultProperty(void *data, std::uint32_t subject,
                                const char *key, const char *,
                                const char *value) {
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  if (subject != PW_ID_CORE || key == nullptr || value == nullptr ||
      !isLegacyDefaultKey(key) || !metadataSelectsLegacySink(value)) {
    return 0;
  }
  const auto result = pw_metadata_set_property(
      runtime.defaultMetadata, PW_ID_CORE, key, nullptr, nullptr);
  if (result < 0) {
    failSetupPipeWireIntegration(
        runtime,
        setupPipeWireSystemError("cannot remove obsolete PipeTune default "
                                 "output selection",
                                 result));
    return 0;
  }
  runtime.legacyClearRequested = true;
  static_cast<void>(requestSetupPipeWireSync(
      runtime, "cannot synchronize obsolete PipeTune default removal"));
  return 0;
}

static void unbindSetupMetadata(pw_metadata *&metadata, spa_hook &listener,
                                bool &listenerInstalled) noexcept {
  if (metadata == nullptr) {
    return;
  }
  if (listenerInstalled) {
    spa_hook_remove(&listener);
    listenerInstalled = false;
  }
  pw_proxy_destroy(reinterpret_cast<pw_proxy *>(metadata));
  metadata = nullptr;
}

static void bindSetupMetadata(SetupPipeWireRuntime &runtime,
                              pw_metadata **destination,
                              std::uint32_t &destinationId,
                              spa_hook &listener,
                              bool &listenerInstalled,
                              pw_metadata_events &events,
                              SetupMetadataPropertyCallback property,
                              std::uint32_t id,
                              std::uint32_t version) {
  if (*destination != nullptr) {
    return;
  }
  *destination = static_cast<pw_metadata *>(pw_registry_bind(
      runtime.registry, id, PW_TYPE_INTERFACE_Metadata,
      std::min(version, static_cast<std::uint32_t>(PW_VERSION_METADATA)), 0));
  if (*destination == nullptr) {
    return;
  }
  events = {};
  events.version = PW_VERSION_METADATA_EVENTS;
  events.property = property;
  const auto result = pw_metadata_add_listener(
      *destination, &listener, &events, &runtime);
  if (result < 0) {
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(*destination));
    *destination = nullptr;
    return;
  }
  destinationId = id;
  listenerInstalled = true;
  static_cast<void>(requestSetupPipeWireSync(
      runtime, "cannot synchronize setup metadata"));
}

static void setupRegistryGlobal(void *data, std::uint32_t id,
                                std::uint32_t, const char *type,
                                std::uint32_t version,
                                const spa_dict *properties) {
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  if (type == nullptr ||
      std::string_view(type) != PW_TYPE_INTERFACE_Metadata) {
    return;
  }
  const auto name =
      setupDictionaryString(properties, PW_KEY_METADATA_NAME);
  if (name == kPolicyMetadataName) {
    bindSetupMetadata(runtime, &runtime.policyMetadata,
                      runtime.policyMetadataId,
                      runtime.policyListener,
                      runtime.policyListenerInstalled,
                      runtime.policyEvents, setupPolicyProperty, id,
                      version);
  } else if (name == kDefaultMetadataName) {
    bindSetupMetadata(runtime, &runtime.defaultMetadata,
                      runtime.defaultMetadataId,
                      runtime.defaultListener,
                      runtime.defaultListenerInstalled,
                      runtime.defaultEvents, setupDefaultProperty, id,
                      version);
  }
}

static void setupRegistryGlobalRemoved(void *data, std::uint32_t id) {
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  // A WirePlumber restart can replace metadata after this client has bound to
  // the previous global. Release that binding so the replacement global event
  // can install a fresh listener.
  if (id == runtime.policyMetadataId) {
    unbindSetupMetadata(runtime.policyMetadata, runtime.policyListener,
                        runtime.policyListenerInstalled);
    runtime.policyMetadataId = PW_ID_ANY;
    runtime.policyProtocol.clear();
    runtime.policyBackend.clear();
    runtime.policyState.clear();
  }
  if (id == runtime.defaultMetadataId) {
    unbindSetupMetadata(runtime.defaultMetadata, runtime.defaultListener,
                        runtime.defaultListenerInstalled);
    runtime.defaultMetadataId = PW_ID_ANY;
    runtime.legacyClearRequested = false;
  }
}

static void setupCoreDone(void *data, std::uint32_t id, int sequence) {
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  if (id != PW_ID_CORE || sequence != runtime.pendingSequence ||
      !runtime.error.empty()) {
    return;
  }
  if (!runtime.registrySynchronized) {
    // Metadata listeners are installed from registry callbacks. Their initial
    // property snapshots can be queued after the registry barrier, so require
    // one complete additional round trip before accepting the handshake.
    runtime.registrySynchronized = true;
    static_cast<void>(requestSetupPipeWireSync(
        runtime, "cannot synchronize setup metadata snapshots"));
    return;
  }
  if (runtime.legacyClearRequested) {
    runtime.legacyDefaultCleared = true;
  }
  if (!setupPolicyReady(runtime)) {
    return;
  }
  runtime.completed = true;
  pw_main_loop_quit(runtime.mainLoop);
}

static void setupCoreError(void *data, std::uint32_t id, int, int result,
                           const char *message) {
  if (id != PW_ID_CORE) {
    return;
  }
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  failSetupPipeWireIntegration(
      runtime,
      message == nullptr
          ? setupPipeWireSystemError("PipeWire setup connection failed",
                                     result)
          : "PipeWire setup connection failed: " + std::string(message));
}

static void setupIntegrationTimedOut(void *data, std::uint64_t) {
  auto &runtime = *static_cast<SetupPipeWireRuntime *>(data);
  if (runtime.policyMetadata == nullptr) {
    failSetupPipeWireIntegration(
        runtime,
        "WirePlumber PipeTune policy handshake is unavailable; verify that "
        "the PipeTune policy files are installed in WirePlumber's active "
        "data directory");
    return;
  }
  failSetupPipeWireIntegration(
      runtime,
      "WirePlumber PipeTune policy handshake is incomplete or incompatible "
      "(protocol=" + runtime.policyProtocol + ", backend=" +
          runtime.policyBackend + ", state=" + runtime.policyState + ")");
}

static bool createSetupPipeWireRuntime(SetupPipeWireRuntime &runtime) {
  runtime.mainLoop = pw_main_loop_new(nullptr);
  if (runtime.mainLoop == nullptr) {
    runtime.error = setupPipeWireSystemError(
        "cannot create setup PipeWire main loop", -errno);
    return false;
  }
  auto *loop = pw_main_loop_get_loop(runtime.mainLoop);
  runtime.timeoutSource =
      pw_loop_add_timer(loop, setupIntegrationTimedOut, &runtime);
  if (runtime.timeoutSource == nullptr) {
    runtime.error = setupPipeWireSystemError(
        "cannot create setup PipeWire timeout", -errno);
    return false;
  }
  auto delay = timespec{.tv_sec = kSetupIntegrationTimeoutSeconds,
                        .tv_nsec = 0};
  auto interval = timespec{.tv_sec = 0, .tv_nsec = 0};
  const auto timerResult = pw_loop_update_timer(
      loop, runtime.timeoutSource, &delay, &interval, false);
  if (timerResult < 0) {
    runtime.error = setupPipeWireSystemError(
        "cannot arm setup PipeWire timeout", timerResult);
    return false;
  }
  runtime.context = pw_context_new(loop, nullptr, 0);
  if (runtime.context == nullptr) {
    runtime.error = setupPipeWireSystemError(
        "cannot create setup PipeWire context", -errno);
    return false;
  }
  runtime.core = pw_context_connect(runtime.context, nullptr, 0);
  if (runtime.core == nullptr) {
    runtime.error = setupPipeWireSystemError(
        "cannot connect to the user's PipeWire session", -errno);
    return false;
  }
  runtime.coreEvents.version = PW_VERSION_CORE_EVENTS;
  runtime.coreEvents.done = setupCoreDone;
  runtime.coreEvents.error = setupCoreError;
  const auto coreListenerResult = pw_core_add_listener(
      runtime.core, &runtime.coreListener, &runtime.coreEvents, &runtime);
  if (coreListenerResult < 0) {
    runtime.error = setupPipeWireSystemError(
        "cannot monitor the setup PipeWire connection", coreListenerResult);
    return false;
  }
  runtime.coreListenerInstalled = true;
  runtime.registry =
      pw_core_get_registry(runtime.core, PW_VERSION_REGISTRY, 0);
  if (runtime.registry == nullptr) {
    runtime.error = setupPipeWireSystemError(
        "cannot access the setup PipeWire registry", -errno);
    return false;
  }
  runtime.registryEvents.version = PW_VERSION_REGISTRY_EVENTS;
  runtime.registryEvents.global = setupRegistryGlobal;
  runtime.registryEvents.global_remove = setupRegistryGlobalRemoved;
  const auto registryListenerResult = pw_registry_add_listener(
      runtime.registry, &runtime.registryListener, &runtime.registryEvents,
      &runtime);
  if (registryListenerResult < 0) {
    runtime.error = setupPipeWireSystemError(
        "cannot monitor the setup PipeWire registry",
        registryListenerResult);
    return false;
  }
  runtime.registryListenerInstalled = true;
  return requestSetupPipeWireSync(
      runtime, "cannot synchronize setup PipeWire registry");
}

static void destroySetupPipeWireRuntime(
    SetupPipeWireRuntime &runtime) noexcept {
  if (runtime.policyMetadata != nullptr) {
    unbindSetupMetadata(runtime.policyMetadata, runtime.policyListener,
                        runtime.policyListenerInstalled);
    runtime.policyMetadataId = PW_ID_ANY;
  }
  if (runtime.defaultMetadata != nullptr) {
    unbindSetupMetadata(runtime.defaultMetadata, runtime.defaultListener,
                        runtime.defaultListenerInstalled);
    runtime.defaultMetadataId = PW_ID_ANY;
  }
  if (runtime.registry != nullptr) {
    if (runtime.registryListenerInstalled) {
      spa_hook_remove(&runtime.registryListener);
      runtime.registryListenerInstalled = false;
    }
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(runtime.registry));
    runtime.registry = nullptr;
  }
  if (runtime.core != nullptr) {
    if (runtime.coreListenerInstalled) {
      spa_hook_remove(&runtime.coreListener);
      runtime.coreListenerInstalled = false;
    }
    pw_core_disconnect(runtime.core);
    runtime.core = nullptr;
  }
  if (runtime.context != nullptr) {
    pw_context_destroy(runtime.context);
    runtime.context = nullptr;
  }
  if (runtime.mainLoop != nullptr) {
    if (runtime.timeoutSource != nullptr) {
      pw_loop_destroy_source(pw_main_loop_get_loop(runtime.mainLoop),
                             runtime.timeoutSource);
      runtime.timeoutSource = nullptr;
    }
    pw_main_loop_destroy(runtime.mainLoop);
    runtime.mainLoop = nullptr;
  }
}

SetupPipeWireIntegrationResult prepareSetupPipeWireIntegration() {
  auto pipeWireScope = SetupPipeWireLibraryScope{};
  auto runtime = SetupPipeWireRuntime{
      .mainLoop = nullptr,
      .context = nullptr,
      .core = nullptr,
      .registry = nullptr,
      .policyMetadata = nullptr,
      .defaultMetadata = nullptr,
      .policyMetadataId = PW_ID_ANY,
      .defaultMetadataId = PW_ID_ANY,
      .coreEvents = {},
      .registryEvents = {},
      .policyEvents = {},
      .defaultEvents = {},
      .coreListener = {},
      .registryListener = {},
      .policyListener = {},
      .defaultListener = {},
      .timeoutSource = nullptr,
      .pendingSequence = 0,
      .coreListenerInstalled = false,
      .registryListenerInstalled = false,
      .policyListenerInstalled = false,
      .defaultListenerInstalled = false,
      .registrySynchronized = false,
      .legacyClearRequested = false,
      .legacyDefaultCleared = false,
      .completed = false,
      .policyProtocol = {},
      .policyBackend = {},
      .policyState = {},
      .error = {},
  };
  if (createSetupPipeWireRuntime(runtime)) {
    const auto runResult = pw_main_loop_run(runtime.mainLoop);
    if (runResult < 0 && runtime.error.empty()) {
      runtime.error = setupPipeWireSystemError(
          "setup PipeWire main loop failed", runResult);
    } else if (!runtime.completed && runtime.error.empty()) {
      runtime.error = "setup PipeWire integration stopped before completion";
    }
  }
  auto result = SetupPipeWireIntegrationResult{
      .policyBackend = runtime.error.empty() ? runtime.policyBackend
                                             : std::string{},
      .legacyDefaultCleared = runtime.legacyDefaultCleared,
      .error = std::move(runtime.error),
  };
  destroySetupPipeWireRuntime(runtime);
  return result;
}

} // namespace pipetune
