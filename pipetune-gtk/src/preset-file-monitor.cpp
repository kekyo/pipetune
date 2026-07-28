#include "preset-file-monitor.h"

#include <gio/gio.h>

#include <algorithm>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace pipetune_gtk {

constexpr auto kPresetRefreshDelayMilliseconds = guint{100};

struct EffeTunePresetFileMonitor {
  std::filesystem::path targetPath;
  GFileMonitor *fileMonitor;
  EffeTunePresetFileChangeCallback callback;
  void *userData;
  guint refreshSource;
  bool monitoringTarget;
  bool destroying;
};

static bool isDirectory(const std::filesystem::path &path) {
  auto error = std::error_code{};
  const auto directory = std::filesystem::is_directory(path, error);
  return directory && !error;
}

static std::filesystem::path nearestExistingDirectory(
    const std::filesystem::path &path) {
  auto candidate = path;
  while (!candidate.empty()) {
    if (isDirectory(candidate)) {
      return candidate;
    }
    const auto parent = candidate.parent_path();
    if (parent == candidate) {
      break;
    }
    candidate = parent;
  }
  return {};
}

static bool isPathPrefix(const std::filesystem::path &prefix,
                         const std::filesystem::path &path) {
  const auto mismatch = std::mismatch(
      prefix.begin(), prefix.end(), path.begin(), path.end());
  return mismatch.first == prefix.end();
}

static std::filesystem::path localFilePath(GFile *file) {
  if (file == nullptr) {
    return {};
  }
  auto *path = g_file_get_path(file);
  if (path == nullptr) {
    return {};
  }
  auto result = std::filesystem::path(path);
  g_free(path);
  return result;
}

static void onFileMonitorChanged(
    GFileMonitor *, GFile *file, GFile *otherFile,
    GFileMonitorEvent eventType, gpointer userData);

static std::string monitorError(
    std::string_view operation, GError *error) {
  auto result = std::string(operation);
  if (error != nullptr) {
    result += ": ";
    result += error->message;
    g_error_free(error);
  }
  return result;
}

static GFileMonitor *createFileMonitor(
    const std::filesystem::path &path, GError **error) {
  auto *file = g_file_new_for_path(path.c_str());
  auto *monitor = g_file_monitor_file(
      file, G_FILE_MONITOR_NONE, nullptr, error);
  g_object_unref(file);
  return monitor;
}

static GFileMonitor *createDirectoryMonitor(
    const std::filesystem::path &path, GError **error) {
  auto *file = g_file_new_for_path(path.c_str());
  auto *monitor = g_file_monitor_directory(
      file, G_FILE_MONITOR_WATCH_MOVES, nullptr, error);
  g_object_unref(file);
  return monitor;
}

static std::string armMonitor(EffeTunePresetFileMonitor *state) {
  // A file monitor handles atomic replacements once the parent exists. Until
  // then, watch the nearest existing ancestor and re-arm when it is created.
  auto monitoredPath = state->targetPath;
  auto monitoringTarget =
      isDirectory(state->targetPath.parent_path());
  auto *error = static_cast<GError *>(nullptr);
  auto *monitor =
      monitoringTarget
          ? createFileMonitor(state->targetPath, &error)
          : nullptr;
  if (monitor == nullptr) {
    if (error != nullptr) {
      g_error_free(error);
      error = nullptr;
    }
    monitoredPath = nearestExistingDirectory(
        state->targetPath.parent_path());
    monitoringTarget = false;
    if (monitoredPath.empty()) {
      return "cannot find an existing directory for EffeTune preset "
             "monitoring";
    }
    monitor = createDirectoryMonitor(monitoredPath, &error);
  }
  if (monitor == nullptr) {
    return monitorError(
        "cannot monitor EffeTune saved presets", error);
  }

  g_file_monitor_set_rate_limit(
      monitor, kPresetRefreshDelayMilliseconds);
  g_signal_connect(monitor, "changed",
                   G_CALLBACK(onFileMonitorChanged), state);
  if (state->fileMonitor != nullptr) {
    g_file_monitor_cancel(state->fileMonitor);
    g_object_unref(state->fileMonitor);
  }
  state->fileMonitor = monitor;
  state->monitoringTarget = monitoringTarget;
  return {};
}

static gboolean notifyPresetFileChanged(gpointer userData) {
  auto *state =
      static_cast<EffeTunePresetFileMonitor *>(userData);
  state->refreshSource = 0;
  if (state->destroying) {
    return G_SOURCE_REMOVE;
  }
  armMonitor(state);
  state->callback(state->userData);
  return G_SOURCE_REMOVE;
}

static bool isRelevantEvent(
    const EffeTunePresetFileMonitor &state, GFile *file,
    GFile *otherFile, GFileMonitorEvent eventType) {
  if (eventType == G_FILE_MONITOR_EVENT_PRE_UNMOUNT ||
      eventType == G_FILE_MONITOR_EVENT_UNMOUNTED) {
    return true;
  }
  if (state.monitoringTarget) {
    return true;
  }
  const auto changedPath = localFilePath(file);
  const auto otherPath = localFilePath(otherFile);
  return (!changedPath.empty() &&
          isPathPrefix(changedPath, state.targetPath)) ||
         (!otherPath.empty() &&
          isPathPrefix(otherPath, state.targetPath));
}

static void onFileMonitorChanged(
    GFileMonitor *, GFile *file, GFile *otherFile,
    GFileMonitorEvent eventType, gpointer userData) {
  auto *state =
      static_cast<EffeTunePresetFileMonitor *>(userData);
  if (state->destroying ||
      !isRelevantEvent(*state, file, otherFile, eventType)) {
    return;
  }
  if (state->refreshSource != 0) {
    g_source_remove(state->refreshSource);
  }
  state->refreshSource = g_timeout_add(
      kPresetRefreshDelayMilliseconds, notifyPresetFileChanged,
      state);
}

EffeTunePresetFileMonitorCreateResult createEffeTunePresetFileMonitor(
    const std::filesystem::path &path,
    EffeTunePresetFileChangeCallback callback, void *userData) {
  if (path.empty() || callback == nullptr) {
    return {
        .monitor = nullptr,
        .error = "EffeTune saved preset monitor arguments are invalid"};
  }
  auto filesystemError = std::error_code{};
  const auto absolutePath =
      std::filesystem::absolute(path, filesystemError)
          .lexically_normal();
  if (filesystemError) {
    return {
        .monitor = nullptr,
        .error = "cannot resolve EffeTune saved preset path: " +
                 filesystemError.message()};
  }

  auto *state = new EffeTunePresetFileMonitor{
      .targetPath = absolutePath,
      .fileMonitor = nullptr,
      .callback = callback,
      .userData = userData,
      .refreshSource = 0,
      .monitoringTarget = false,
      .destroying = false,
  };
  const auto error = armMonitor(state);
  if (!error.empty()) {
    delete state;
    return {.monitor = nullptr, .error = error};
  }
  return {.monitor = state, .error = {}};
}

void destroyEffeTunePresetFileMonitor(
    EffeTunePresetFileMonitor *monitor) {
  if (monitor == nullptr) {
    return;
  }
  monitor->destroying = true;
  if (monitor->refreshSource != 0) {
    g_source_remove(monitor->refreshSource);
    monitor->refreshSource = 0;
  }
  if (monitor->fileMonitor != nullptr) {
    g_file_monitor_cancel(monitor->fileMonitor);
    g_object_unref(monitor->fileMonitor);
    monitor->fileMonitor = nullptr;
  }
  delete monitor;
}

} // namespace pipetune_gtk
