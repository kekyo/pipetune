/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "autostart_override.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace pipetune {

constexpr auto kManagedMarker =
    std::string_view{"X-PipeTune-Managed-Autostart-Mask=true"};
constexpr auto kManagedMask =
    std::string_view{"[Desktop Entry]\n"
                     "Type=Application\n"
                     "Hidden=true\n"
                     "X-PipeTune-Managed-Autostart-Mask=true\n"};
constexpr auto kMaximumAutostartBytes = std::size_t{64 * 1024};

static bool pathExists(const std::filesystem::path &path,
                       std::error_code &error) {
  const auto status = std::filesystem::symlink_status(path, error);
  if (error == std::errc::no_such_file_or_directory) {
    error.clear();
    return false;
  }
  return !error && status.type() != std::filesystem::file_type::not_found;
}

static std::string systemError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static std::string writeAll(int descriptor, std::string_view contents) {
  auto offset = std::size_t{0};
  while (offset < contents.size()) {
    const auto count =
        write(descriptor, contents.data() + offset, contents.size() - offset);
    if (count > 0) {
      offset += static_cast<std::size_t>(count);
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return systemError("cannot write GTK autostart mask");
  }
  return {};
}

static std::string writeManagedMask(
    const std::filesystem::path &target) {
  const auto directory = target.parent_path();
  if (directory.empty()) {
    return "GTK autostart override requires a parent directory";
  }
  auto filesystemError = std::error_code{};
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    return "cannot create GTK autostart directory: " +
           filesystemError.message();
  }
  if (chmod(directory.c_str(), 0700) != 0) {
    return systemError("cannot secure GTK autostart directory");
  }

  auto templatePath =
      (directory / ".pipetune-autostart.XXXXXX").string();
  auto templateBuffer =
      std::vector<char>(templatePath.begin(), templatePath.end());
  templateBuffer.push_back('\0');
  const auto descriptor = mkstemp(templateBuffer.data());
  if (descriptor < 0) {
    return systemError("cannot create temporary GTK autostart mask");
  }
  const auto temporaryPath = std::filesystem::path(templateBuffer.data());
  auto error = std::string{};
  if (fchmod(descriptor, 0600) != 0) {
    error = systemError("cannot secure GTK autostart mask");
  }
  if (error.empty()) {
    error = writeAll(descriptor, kManagedMask);
  }
  if (error.empty() && fsync(descriptor) != 0) {
    error = systemError("cannot synchronize GTK autostart mask");
  }
  if (close(descriptor) != 0 && error.empty()) {
    error = systemError("cannot close GTK autostart mask");
  }
  if (error.empty() &&
      rename(temporaryPath.c_str(), target.c_str()) != 0) {
    error = systemError("cannot replace GTK autostart override");
  }
  if (!error.empty()) {
    unlink(temporaryPath.c_str());
  }
  return error;
}

bool isPipeTuneManagedAutostartMask(
    const std::filesystem::path &path) {
  auto stream = std::ifstream(path, std::ios::binary);
  if (!stream) {
    return false;
  }
  auto contents = std::string{};
  auto buffer = std::vector<char>(4096);
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      contents.append(buffer.data(), static_cast<std::size_t>(count));
      if (contents.size() > kMaximumAutostartBytes) {
        return false;
      }
    }
  }
  if (stream.bad()) {
    return false;
  }
  auto offset = std::size_t{0};
  while (offset <= contents.size()) {
    const auto end = contents.find('\n', offset);
    const auto length =
        end == std::string::npos ? contents.size() - offset : end - offset;
    auto line = std::string_view(contents).substr(offset, length);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (line == kManagedMarker) {
      return true;
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return false;
}

AutostartUpdateResult
maskGtkAutostart(const std::filesystem::path &target,
                 const std::filesystem::path &backup) {
  auto filesystemError = std::error_code{};
  const auto targetExists = pathExists(target, filesystemError);
  if (filesystemError) {
    return {.success = false,
            .warnings = {},
            .error = "cannot inspect GTK autostart override: " +
                     filesystemError.message()};
  }
  if (targetExists && isPipeTuneManagedAutostartMask(target)) {
    return {.success = true, .warnings = {}, .error = {}};
  }

  auto movedCustomOverride = false;
  if (targetExists) {
    const auto backupExists = pathExists(backup, filesystemError);
    if (filesystemError) {
      return {.success = false,
              .warnings = {},
              .error = "cannot inspect GTK autostart backup: " +
                       filesystemError.message()};
    }
    if (backupExists) {
      return {.success = false,
              .warnings = {},
              .error = "cannot mask GTK autostart because its backup "
                       "already exists"};
    }
    std::filesystem::rename(target, backup, filesystemError);
    if (filesystemError) {
      return {.success = false,
              .warnings = {},
              .error = "cannot back up GTK autostart override: " +
                       filesystemError.message()};
    }
    movedCustomOverride = true;
  }

  const auto writeError = writeManagedMask(target);
  if (!writeError.empty()) {
    if (movedCustomOverride) {
      auto rollbackError = std::error_code{};
      std::filesystem::rename(backup, target, rollbackError);
      if (rollbackError) {
        return {.success = false,
                .warnings = {
                    "GTK autostart override backup remains at " +
                    backup.string()},
                .error = writeError};
      }
    }
    return {.success = false, .warnings = {}, .error = writeError};
  }
  return {.success = true, .warnings = {}, .error = {}};
}

AutostartUpdateResult
restoreGtkAutostart(const std::filesystem::path &target,
                    const std::filesystem::path &backup) {
  auto filesystemError = std::error_code{};
  const auto targetExists = pathExists(target, filesystemError);
  if (filesystemError) {
    return {.success = false,
            .warnings = {},
            .error = "cannot inspect GTK autostart override: " +
                     filesystemError.message()};
  }
  const auto backupExists = pathExists(backup, filesystemError);
  if (filesystemError) {
    return {.success = false,
            .warnings = {},
            .error = "cannot inspect GTK autostart backup: " +
                     filesystemError.message()};
  }

  if (!targetExists) {
    auto warnings = std::vector<std::string>{};
    if (backupExists) {
      warnings.push_back("orphaned GTK autostart backup was preserved at " +
                         backup.string());
    }
    return {.success = true,
            .warnings = std::move(warnings),
            .error = {}};
  }
  if (!isPipeTuneManagedAutostartMask(target)) {
    auto warnings = std::vector<std::string>{
        "unmanaged GTK autostart override was preserved at " +
        target.string()};
    if (backupExists) {
      warnings.push_back("GTK autostart backup was also preserved at " +
                         backup.string());
    }
    return {.success = true,
            .warnings = std::move(warnings),
            .error = {}};
  }

  std::filesystem::remove(target, filesystemError);
  if (filesystemError) {
    return {.success = false,
            .warnings = {},
            .error = "cannot remove managed GTK autostart mask: " +
                     filesystemError.message()};
  }
  if (backupExists) {
    std::filesystem::rename(backup, target, filesystemError);
    if (filesystemError) {
      return {.success = false,
              .warnings = {
                  "GTK autostart backup remains at " + backup.string()},
              .error = "cannot restore GTK autostart override: " +
                       filesystemError.message()};
    }
  }
  return {.success = true, .warnings = {}, .error = {}};
}

} // namespace pipetune
