#include "startup-config.h"

#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace pipetune_gtk {

constexpr auto kPresetAssignment = std::string_view{"PIPETUNE_PRESET="};
constexpr auto kMaximumConfigBytes = std::size_t{64 * 1024};

static std::string systemError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static std::string encodePresetPath(std::string_view path) {
  auto encoded = std::string{};
  encoded.reserve(path.size() + 2);
  encoded.push_back('"');
  for (const auto character : path) {
    if (character == '\\' || character == '"') {
      encoded.push_back('\\');
    }
    encoded.push_back(character);
  }
  encoded.push_back('"');
  return encoded;
}

static bool decodePresetPath(std::string_view value, std::string &decoded) {
  if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
    return false;
  }
  decoded.clear();
  decoded.reserve(value.size() - 2);
  auto escaped = false;
  for (auto index = std::size_t{1}; index + 1 < value.size(); ++index) {
    const auto character = value[index];
    if (escaped) {
      if (character != '\\' && character != '"') {
        return false;
      }
      decoded.push_back(character);
      escaped = false;
    } else if (character == '\\') {
      escaped = true;
    } else {
      decoded.push_back(character);
    }
  }
  return !escaped;
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
    return systemError("cannot write startup preset override");
  }
  return {};
}

StartupConfigPathResult
resolveStartupConfigPath(std::string_view xdgConfigHome,
                         const std::filesystem::path &homeDirectory) {
  if (!xdgConfigHome.empty()) {
    return {.path = std::filesystem::path(xdgConfigHome) / "pipetune" /
                    "environment.gtk",
            .error = {}};
  }
  if (homeDirectory.empty()) {
    return {.path = {},
            .error = "HOME is required when XDG_CONFIG_HOME is unset"};
  }
  return {.path = homeDirectory / ".config" / "pipetune" /
                  "environment.gtk",
          .error = {}};
}

StartupPresetLoadResult
loadStartupPreset(const std::filesystem::path &configPath) {
  auto stream = std::ifstream(configPath, std::ios::binary);
  if (!stream) {
    if (!std::filesystem::exists(configPath)) {
      return {.found = false, .presetPath = {}, .error = {}};
    }
    return {.found = false,
            .presetPath = {},
            .error = "cannot read startup preset override"};
  }

  auto contents = std::string(
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>());
  if (stream.bad() || contents.size() > kMaximumConfigBytes) {
    return {.found = false,
            .presetPath = {},
            .error = "startup preset override is unreadable or too large"};
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
    if (line.starts_with(kPresetAssignment)) {
      auto decoded = std::string{};
      if (!decodePresetPath(line.substr(kPresetAssignment.size()), decoded)) {
        return {.found = false,
                .presetPath = {},
                .error = "startup preset assignment is invalid"};
      }
      auto presetPath = std::filesystem::path(decoded);
      if (!presetPath.is_absolute() || decoded.find('\0') != std::string::npos ||
          decoded.find('\n') != std::string::npos ||
          decoded.find('\r') != std::string::npos) {
        return {.found = false,
                .presetPath = {},
                .error = "startup preset path must be an absolute single "
                         "line"};
      }
      return {.found = true,
              .presetPath = std::move(presetPath),
              .error = {}};
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return {.found = false, .presetPath = {}, .error = {}};
}

std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath) {
  const auto nativePreset = presetPath.string();
  if (!presetPath.is_absolute() || nativePreset.empty() ||
      nativePreset.find('\0') != std::string::npos ||
      nativePreset.find('\n') != std::string::npos ||
      nativePreset.find('\r') != std::string::npos) {
    return "startup preset path must be an absolute single line";
  }

  auto filesystemError = std::error_code{};
  const auto directory = configPath.parent_path();
  if (directory.empty()) {
    return "startup preset override requires a parent directory";
  }
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    return "cannot create startup preset directory: " +
           filesystemError.message();
  }
  if (chmod(directory.c_str(), 0700) != 0) {
    return systemError("cannot secure startup preset directory");
  }

  auto templatePath = (directory / ".environment.gtk.XXXXXX").string();
  auto templateBuffer =
      std::vector<char>(templatePath.begin(), templatePath.end());
  templateBuffer.push_back('\0');
  const auto descriptor = mkstemp(templateBuffer.data());
  if (descriptor < 0) {
    return systemError("cannot create temporary startup preset override");
  }
  const auto temporaryPath = std::filesystem::path(templateBuffer.data());
  auto error = std::string{};
  if (fchmod(descriptor, 0600) != 0) {
    error = systemError("cannot secure startup preset override");
  }
  const auto contents =
      std::string("# Managed by pipetune-gtk.\n") +
      std::string(kPresetAssignment) + encodePresetPath(nativePreset) + "\n";
  if (error.empty()) {
    error = writeAll(descriptor, contents);
  }
  if (error.empty() && fsync(descriptor) != 0) {
    error = systemError("cannot synchronize startup preset override");
  }
  if (close(descriptor) != 0 && error.empty()) {
    error = systemError("cannot close startup preset override");
  }
  if (error.empty() &&
      rename(temporaryPath.c_str(), configPath.c_str()) != 0) {
    error = systemError("cannot replace startup preset override");
  }
  if (!error.empty()) {
    unlink(temporaryPath.c_str());
  }
  return error;
}

} // namespace pipetune_gtk
