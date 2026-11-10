/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipewire/pipewire.h>
#include <dlfcn.h>

#include <cerrno>
#include <memory>
#include <new>

struct DistributionStreamPolicy {
  pw_stream *stream;
  spa_hook listener{};
};

/**
 * Delegates stream creation and attaches suspend cleanup to PipeTune's distribution outputs.
 * @param core Borrowed PipeWire connection.
 * @param name Stream name passed unchanged to libpipewire.
 * @param properties Owned properties, consumed on both success and failure.
 * @return The real PipeWire stream, or null with errno set.
 * @remarks Exported for calls made by the dynamically loaded combine module.
 * Only public stream APIs are used; other streams retain normal library behavior.
 */
extern "C" pw_stream *pw_stream_new(pw_core *core, const char *name, pw_properties *properties) {
  static const auto create = [] {
    dlerror();
    const auto function = reinterpret_cast<decltype(&pw_stream_new)>(dlsym(RTLD_NEXT, "pw_stream_new"));
    return dlerror() == nullptr ? function : nullptr;
  }();
  if (create == nullptr) {
    if (properties != nullptr) pw_properties_free(properties);
    errno = ENOSYS;
    return nullptr;
  }
  if (properties == nullptr || !pw_properties_get_bool(properties, "node.pipetune.managed-output", false) ||
      pw_properties_get(properties, "node.pipetune.output-id") == nullptr)
    return create(core, name, properties);

  auto policy = std::unique_ptr<DistributionStreamPolicy>(new (std::nothrow) DistributionStreamPolicy{});
  if (!policy) {
    pw_properties_free(properties);
    errno = ENOMEM;
    return nullptr;
  }
  auto *stream = create(core, name, properties);
  if (stream == nullptr) return nullptr;
  policy->stream = stream;
  static const auto events = [] {
    auto value = pw_stream_events{};
    value.version = PW_VERSION_STREAM_EVENTS;
    value.state_changed = [](void *data, pw_stream_state, pw_stream_state state, const char *) {
      if (state != PW_STREAM_STATE_PAUSED) return;
      auto &policy = *static_cast<DistributionStreamPolicy *>(data);
      // Combine clears its delay lines on suspend but retains queued output
      // buffers. A startup queue imbalance can therefore survive into the next
      // activation, shifting one device and replaying the previous source.
      // Match the public flush boundary used by PipeWire's loopback module.
      // https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-combine-stream.c
      // https://github.com/PipeWire/pipewire/blob/1.0.5/src/modules/module-loopback.c
      const auto result = pw_stream_flush(policy.stream, false);
      if (result < 0) pw_stream_set_error(policy.stream, result, "cannot discard suspended distribution audio");
    };
    value.destroy = [](void *data) {
      auto *policy = static_cast<DistributionStreamPolicy *>(data);
      spa_hook_remove(&policy->listener);
      delete policy;
    };
    return value;
  }();
  pw_stream_add_listener(stream, &policy->listener, &events, policy.get());
  static_cast<void>(policy.release());
  return stream;
}
