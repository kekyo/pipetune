/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "active_preset_file_monitor.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pipetune {

static constexpr auto kDirectoryEventMask =
    std::uint32_t{IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                  IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF};
static constexpr auto kFileEventMask =
    std::uint32_t{IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF};

struct ActivePresetFileMonitor::Impl {
  int descriptor;
  std::filesystem::path targetPath;
  std::vector<std::filesystem::path> targets;
  std::map<std::filesystem::path, int> watches;

  explicit Impl(int value) : descriptor(value) {}
  ~Impl() { if (descriptor >= 0) close(descriptor); }
};

static std::string systemError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static bool containsTarget(const std::filesystem::path &parent,
                            const std::filesystem::path &target) {
  const auto relative = target.lexically_relative(parent);
  return !relative.empty() && *relative.begin() != "..";
}

static std::string armWatches(ActivePresetFileMonitor::Impl &state) {
  auto desired = std::map<std::filesystem::path, std::uint32_t>{};
  for (const auto &target : state.targets) {
    auto error = std::error_code{};
    if (std::filesystem::exists(target, error)) desired[target] |= kFileEventMask;
    auto directory = target.parent_path();
    // Watch the nearest existing ancestor until measurement-backups is created.
    // Keep its parent watched as well so directory replacement can be recovered.
    while (!directory.empty() && !std::filesystem::is_directory(directory, error)) {
      const auto parent = directory.parent_path();
      if (parent == directory) break;
      directory = parent;
    }
    if (!directory.empty()) {
      desired[directory] |= kDirectoryEventMask;
      const auto parent = directory.parent_path();
      if (!parent.empty()) desired[parent] |= kDirectoryEventMask;
    }
  }
  // Retain unchanged watches: removing/readding them would lose queued writes.
  for (auto it = state.watches.begin(); it != state.watches.end();) {
    if (!desired.contains(it->first)) {
      static_cast<void>(inotify_rm_watch(state.descriptor, it->second));
      it = state.watches.erase(it);
    } else ++it;
  }
  for (const auto &[path, mask] : desired) {
    const auto watch = inotify_add_watch(state.descriptor, path.c_str(), mask);
    if (watch < 0) {
      // A concurrent rename/unlink is normal; the ancestor watch catches recovery.
      if (errno == ENOENT || errno == ENOTDIR) continue;
      return systemError("cannot monitor preset dependency");
    }
    state.watches[path] = watch;
  }
  return {};
}

ActivePresetFileMonitor::ActivePresetFileMonitor(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}
ActivePresetFileMonitor::~ActivePresetFileMonitor() = default;

int ActivePresetFileMonitor::descriptor() const noexcept {
  return implementation_ == nullptr ? -1 : implementation_->descriptor;
}

std::string ActivePresetFileMonitor::setPath(const std::filesystem::path &path) {
  return setPaths(path, {});
}

std::string ActivePresetFileMonitor::setPaths(
    const std::filesystem::path &path,
    std::span<const std::filesystem::path> dependencies) {
  if (implementation_ == nullptr || path.empty()) return "active preset monitor path must not be empty";
  auto targets = std::vector<std::filesystem::path>{path};
  targets.insert(targets.end(), dependencies.begin(), dependencies.end());
  for (auto &target : targets) {
    auto error = std::error_code{};
    target = std::filesystem::absolute(target, error).lexically_normal();
    if (error) return "cannot resolve preset dependency: " + error.message();
  }
  implementation_->targetPath = targets.front();
  implementation_->targets = std::move(targets);
  return armWatches(*implementation_);
}

void ActivePresetFileMonitor::clear() noexcept {
  if (implementation_ == nullptr) return;
  for (const auto &[path, watch] : implementation_->watches) {
    static_cast<void>(path);
    static_cast<void>(inotify_rm_watch(implementation_->descriptor, watch));
  }
  implementation_->watches.clear();
  implementation_->targets.clear();
  implementation_->targetPath.clear();
}

ActivePresetFileMonitorEvent ActivePresetFileMonitor::consume() {
  auto result = ActivePresetFileMonitorEvent{
      .changed = false,
      .path = implementation_ == nullptr ? std::filesystem::path{} : implementation_->targetPath,
      .error = {}};
  if (implementation_ == nullptr || implementation_->descriptor < 0) {
    result.error = "active preset monitor is unavailable"; return result;
  }
  auto &state = *implementation_;
  alignas(inotify_event) auto buffer = std::array<std::byte, 16 * 1024>{};
  while (true) {
    const auto count = read(state.descriptor, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) result.error = systemError("cannot read active preset monitor");
      break;
    }
    if (count == 0) break;
    const auto size = static_cast<std::size_t>(count);
    for (std::size_t offset = 0; offset + sizeof(inotify_event) <= size;) {
      const auto *event = reinterpret_cast<const inotify_event *>(buffer.data() + offset);
      const auto eventSize = sizeof(inotify_event) + event->len;
      if (offset + eventSize > size) { result.error = "active preset monitor returned an incomplete event"; break; }
      if ((event->mask & IN_Q_OVERFLOW) != 0) result.changed = true;
      else if ((event->mask & kDirectoryEventMask) != 0) {
        for (const auto &[path, watch] : state.watches) {
          if (event->wd != watch) continue;
          const auto changed = event->len == 0 ? path : path / std::string(event->name, strnlen(event->name, event->len));
          for (const auto &target : state.targets) {
            if (changed == target || ((event->mask & (IN_ISDIR | IN_DELETE_SELF | IN_MOVE_SELF)) != 0 && containsTarget(changed, target))) result.changed = true;
          }
        }
      }
      offset += eventSize;
    }
  }
  if (result.changed) {
    const auto error = armWatches(state);
    if (result.error.empty()) result.error = error;
  }
  return result;
}

ActivePresetFileMonitorCreateResult createActivePresetFileMonitor(const std::filesystem::path &path) {
  const auto descriptor = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (descriptor < 0) return {.monitor = nullptr, .error = systemError("cannot create active preset monitor")};
  auto monitor = std::unique_ptr<ActivePresetFileMonitor>(new ActivePresetFileMonitor(std::make_unique<ActivePresetFileMonitor::Impl>(descriptor)));
  if (!path.empty()) {
    const auto error = monitor->setPath(path);
    if (!error.empty()) return {.monitor = nullptr, .error = error};
  }
  return {.monitor = std::move(monitor), .error = {}};
}
} // namespace pipetune
