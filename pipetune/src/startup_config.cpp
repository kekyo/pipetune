#include "pipetune/startup_config.h"

#include <array>
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

static bool decodeQuotedPresetPath(std::string_view value,
                                   std::string &decoded) {
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
    } else if (character == '"') {
      return false;
    } else {
      decoded.push_back(character);
    }
  }
  return !escaped;
}

static bool decodePresetPath(std::string_view value, std::string &decoded) {
  if (value.empty()) {
    return false;
  }
  if (value.front() == '"') {
    return decodeQuotedPresetPath(value, decoded);
  }
  if (value.find('"') != std::string_view::npos) {
    return false;
  }
  decoded.assign(value);
  return true;
}

static std::string validatePresetPath(
    const std::filesystem::path &presetPath) {
  const auto nativePreset = presetPath.string();
  if (!presetPath.is_absolute() || nativePreset.empty() ||
      nativePreset.find('\0') != std::string::npos ||
      nativePreset.find('\n') != std::string::npos ||
      nativePreset.find('\r') != std::string::npos) {
    return "startup preset path must be an absolute single line";
  }
  return {};
}

static std::string readConfig(const std::filesystem::path &configPath,
                              std::string &contents, bool &found) {
  auto stream = std::ifstream(configPath, std::ios::binary);
  if (!stream) {
    auto filesystemError = std::error_code{};
    const auto exists = std::filesystem::exists(configPath, filesystemError);
    if (!exists && !filesystemError) {
      found = false;
      contents.clear();
      return {};
    }
    return "cannot read startup configuration";
  }

  found = true;
  contents.clear();
  auto buffer = std::array<char, 4096>{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      contents.append(buffer.data(), static_cast<std::size_t>(count));
      if (contents.size() > kMaximumConfigBytes) {
        return "startup configuration exceeds 64 KiB";
      }
    }
  }
  if (stream.bad()) {
    return "cannot read startup configuration";
  }
  return {};
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
    return systemError("cannot write startup configuration");
  }
  return {};
}

static std::string writeStartupConfig(
    const std::filesystem::path &configPath,
    const std::filesystem::path *presetPath) {
  auto contents = std::string("# Managed by PipeTune.\n");
  if (presetPath != nullptr) {
    const auto validation = validatePresetPath(*presetPath);
    if (!validation.empty()) {
      return validation;
    }
    contents += std::string(kPresetAssignment) +
                encodePresetPath(presetPath->string()) + "\n";
  }

  auto filesystemError = std::error_code{};
  const auto directory = configPath.parent_path();
  if (directory.empty()) {
    return "startup configuration requires a parent directory";
  }
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    return "cannot create startup configuration directory: " +
           filesystemError.message();
  }
  if (chmod(directory.c_str(), 0700) != 0) {
    return systemError("cannot secure startup configuration directory");
  }

  auto templatePath = (directory / ".environment.XXXXXX").string();
  auto templateBuffer =
      std::vector<char>(templatePath.begin(), templatePath.end());
  templateBuffer.push_back('\0');
  const auto descriptor = mkstemp(templateBuffer.data());
  if (descriptor < 0) {
    return systemError("cannot create temporary startup configuration");
  }
  const auto temporaryPath = std::filesystem::path(templateBuffer.data());
  auto error = std::string{};
  if (fchmod(descriptor, 0600) != 0) {
    error = systemError("cannot secure startup configuration");
  }
  if (error.empty()) {
    error = writeAll(descriptor, contents);
  }
  if (error.empty() && fsync(descriptor) != 0) {
    error = systemError("cannot synchronize startup configuration");
  }
  if (close(descriptor) != 0 && error.empty()) {
    error = systemError("cannot close startup configuration");
  }
  if (error.empty() &&
      rename(temporaryPath.c_str(), configPath.c_str()) != 0) {
    error = systemError("cannot replace startup configuration");
  }
  if (!error.empty()) {
    unlink(temporaryPath.c_str());
  }
  return error;
}

StartupConfigPathResult
resolveStartupConfigPath(std::string_view xdgConfigHome,
                         const std::filesystem::path &homeDirectory) {
  if (!xdgConfigHome.empty()) {
    return {.path = std::filesystem::path(xdgConfigHome) / "pipetune" /
                    "environment",
            .error = {}};
  }
  if (homeDirectory.empty()) {
    return {.path = {},
            .error = "HOME is required when XDG_CONFIG_HOME is unset"};
  }
  return {.path = homeDirectory / ".config" / "pipetune" / "environment",
          .error = {}};
}

StartupPresetLoadResult
loadStartupPreset(const std::filesystem::path &configPath) {
  auto contents = std::string{};
  auto configFound = false;
  const auto readError = readConfig(configPath, contents, configFound);
  if (!readError.empty()) {
    return {.found = false, .presetPath = {}, .error = readError};
  }
  if (!configFound) {
    return {.found = false, .presetPath = {}, .error = {}};
  }

  auto assignmentFound = false;
  auto presetPath = std::filesystem::path{};
  auto offset = std::size_t{0};
  while (offset <= contents.size()) {
    const auto end = contents.find('\n', offset);
    const auto length =
        end == std::string::npos ? contents.size() - offset : end - offset;
    auto line = std::string_view(contents).substr(offset, length);
    if (!line.empty() && line.back() == '\r') {
      line.remove_suffix(1);
    }
    if (!line.empty() && !line.starts_with('#')) {
      if (!line.starts_with(kPresetAssignment)) {
        return {.found = false,
                .presetPath = {},
                .error = "startup configuration contains an unsupported line"};
      }
      if (assignmentFound) {
        return {.found = false,
                .presetPath = {},
                .error = "startup configuration contains duplicate "
                         "PIPETUNE_PRESET assignments"};
      }
      auto decoded = std::string{};
      if (!decodePresetPath(line.substr(kPresetAssignment.size()), decoded)) {
        return {.found = false,
                .presetPath = {},
                .error = "startup preset assignment is invalid"};
      }
      presetPath = std::filesystem::path(decoded);
      const auto validation = validatePresetPath(presetPath);
      if (!validation.empty()) {
        return {.found = false,
                .presetPath = {},
                .error = validation};
      }
      assignmentFound = true;
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return {.found = assignmentFound,
          .presetPath = std::move(presetPath),
          .error = {}};
}

std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath) {
  return writeStartupConfig(configPath, &presetPath);
}

std::string clearStartupPreset(const std::filesystem::path &configPath) {
  return writeStartupConfig(configPath, nullptr);
}

} // namespace pipetune
