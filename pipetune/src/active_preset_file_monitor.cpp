#include "active_preset_file_monitor.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <unistd.h>
#include <utility>

namespace pipetune {

constexpr auto kDirectoryEventMask =
    std::uint32_t{IN_CLOSE_WRITE | IN_MOVED_TO | IN_CREATE | IN_DELETE |
                  IN_MOVED_FROM | IN_DELETE_SELF | IN_MOVE_SELF};
constexpr auto kFileEventMask =
    std::uint32_t{IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF};

struct ActivePresetFileMonitor::Impl {
  int descriptor;
  int directoryWatch;
  int fileWatch;
  std::filesystem::path targetPath;
  std::string targetName;

  explicit Impl(int monitorDescriptor)
      : descriptor(monitorDescriptor), directoryWatch(-1), fileWatch(-1),
        targetPath(), targetName() {}

  ~Impl() {
    if (descriptor >= 0) {
      close(descriptor);
    }
  }
};

static std::string systemError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static void removeWatch(ActivePresetFileMonitor::Impl &state,
                        int &watch) noexcept {
  if (watch >= 0) {
    static_cast<void>(inotify_rm_watch(state.descriptor, watch));
    watch = -1;
  }
}

static void rearmFileWatch(ActivePresetFileMonitor::Impl &state) noexcept {
  removeWatch(state, state.fileWatch);
  if (state.targetPath.empty()) {
    return;
  }
  state.fileWatch =
      inotify_add_watch(state.descriptor, state.targetPath.c_str(),
                        static_cast<std::uint32_t>(kFileEventMask));
}

static bool eventNameMatches(const inotify_event &event,
                             std::string_view targetName) {
  if (event.len == 0) {
    return false;
  }
  const auto length = strnlen(event.name, event.len);
  return std::string_view(event.name, length) == targetName;
}

static bool fileEventIsRelevant(std::uint32_t mask) noexcept {
  return (mask & kFileEventMask) != 0;
}

static bool directoryEventIsRelevant(std::uint32_t mask) noexcept {
  return (mask & kDirectoryEventMask) != 0;
}

ActivePresetFileMonitor::ActivePresetFileMonitor(
    std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

ActivePresetFileMonitor::~ActivePresetFileMonitor() = default;

int ActivePresetFileMonitor::descriptor() const noexcept {
  return implementation_ == nullptr ? -1 : implementation_->descriptor;
}

std::string
ActivePresetFileMonitor::setPath(const std::filesystem::path &path) {
  if (implementation_ == nullptr || path.empty()) {
    return "active preset monitor path must not be empty";
  }
  auto filesystemError = std::error_code{};
  const auto absolutePath =
      std::filesystem::absolute(path, filesystemError).lexically_normal();
  if (filesystemError) {
    return "cannot resolve active preset monitor path: " +
           filesystemError.message();
  }
  if (absolutePath == implementation_->targetPath) {
    rearmFileWatch(*implementation_);
    return {};
  }

  clear();
  implementation_->targetPath = absolutePath;
  implementation_->targetName = absolutePath.filename().string();
  const auto parent = absolutePath.parent_path();
  implementation_->directoryWatch =
      inotify_add_watch(implementation_->descriptor, parent.c_str(),
                        static_cast<std::uint32_t>(kDirectoryEventMask));
  if (implementation_->directoryWatch < 0) {
    const auto error = systemError("cannot monitor active preset directory");
    clear();
    return error;
  }
  rearmFileWatch(*implementation_);
  return {};
}

void ActivePresetFileMonitor::clear() noexcept {
  if (implementation_ == nullptr) {
    return;
  }
  removeWatch(*implementation_, implementation_->fileWatch);
  removeWatch(*implementation_, implementation_->directoryWatch);
  implementation_->targetPath.clear();
  implementation_->targetName.clear();
}

ActivePresetFileMonitorEvent ActivePresetFileMonitor::consume() {
  auto result = ActivePresetFileMonitorEvent{
      .changed = false,
      .path = implementation_ == nullptr ? std::filesystem::path{}
                                        : implementation_->targetPath,
      .error = {},
  };
  if (implementation_ == nullptr || implementation_->descriptor < 0) {
    result.error = "active preset monitor is unavailable";
    return result;
  }

  alignas(inotify_event) auto buffer = std::array<std::byte, 16 * 1024>{};
  while (true) {
    const auto count =
        read(implementation_->descriptor, buffer.data(), buffer.size());
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        result.error = systemError("cannot read active preset monitor");
      }
      break;
    }
    if (count == 0) {
      break;
    }

    auto offset = std::size_t{0};
    const auto size = static_cast<std::size_t>(count);
    while (offset + sizeof(inotify_event) <= size) {
      const auto *event = reinterpret_cast<const inotify_event *>(
          buffer.data() + offset);
      const auto eventSize = sizeof(inotify_event) + event->len;
      if (offset + eventSize > size) {
        result.error = "active preset monitor returned an incomplete event";
        break;
      }
      if ((event->mask & IN_Q_OVERFLOW) != 0) {
        result.changed = true;
      } else if (event->wd == implementation_->fileWatch &&
                 fileEventIsRelevant(event->mask)) {
        result.changed = true;
      } else if (event->wd == implementation_->directoryWatch &&
                 directoryEventIsRelevant(event->mask) &&
                 (eventNameMatches(*event, implementation_->targetName) ||
                  (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF)) != 0)) {
        result.changed = true;
      }
      offset += eventSize;
    }
  }

  if (result.changed && !implementation_->targetPath.empty()) {
    rearmFileWatch(*implementation_);
  }
  return result;
}

ActivePresetFileMonitorCreateResult
createActivePresetFileMonitor(const std::filesystem::path &path) {
  const auto descriptor = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
  if (descriptor < 0) {
    return {.monitor = nullptr,
            .error = systemError("cannot create active preset monitor")};
  }
  auto monitor = std::unique_ptr<ActivePresetFileMonitor>(
      new ActivePresetFileMonitor(
          std::make_unique<ActivePresetFileMonitor::Impl>(descriptor)));
  if (!path.empty()) {
    const auto error = monitor->setPath(path);
    if (!error.empty()) {
      return {.monitor = nullptr, .error = error};
    }
  }
  return {.monitor = std::move(monitor), .error = {}};
}

} // namespace pipetune
