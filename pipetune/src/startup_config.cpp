#include "pipetune/startup_config.h"

#include <array>
#include <charconv>
#include <cerrno>
#include <cstdint>
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
constexpr auto kTargetAssignment = std::string_view{"PIPETUNE_TARGET="};
constexpr auto kDspBackendAssignment =
    std::string_view{"PIPETUNE_DSP_BACKEND="};
constexpr auto kRateAssignment = std::string_view{"PIPETUNE_RATE="};
constexpr auto kRateEnforcementAssignment =
    std::string_view{"PIPETUNE_RATE_ENFORCEMENT="};
constexpr auto kMaximumConfigBytes = std::size_t{64 * 1024};

static std::string systemError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static std::string encodeConfigValue(std::string_view value) {
  auto encoded = std::string{};
  encoded.reserve(value.size() + 2);
  encoded.push_back('"');
  for (const auto character : value) {
    if (character == '\\' || character == '"') {
      encoded.push_back('\\');
    }
    encoded.push_back(character);
  }
  encoded.push_back('"');
  return encoded;
}

static bool decodeQuotedConfigValue(std::string_view value,
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

static bool decodeConfigValue(std::string_view value, std::string &decoded) {
  if (value.empty()) {
    return false;
  }
  if (value.front() == '"') {
    return decodeQuotedConfigValue(value, decoded);
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

static std::string validatePreferredOutput(std::string_view nodeName) {
  if (nodeName.empty() || nodeName.find('\0') != std::string_view::npos ||
      nodeName.find('\n') != std::string_view::npos ||
      nodeName.find('\r') != std::string_view::npos) {
    return "preferred output must be a non-empty single line";
  }
  return {};
}

static bool parseConfiguredRate(std::string_view value,
                                SampleRatePolicy &policy) {
  if (value == "max") {
    policy.mode = SampleRateMode::maximum;
    policy.fixedRate = 0;
    return true;
  }
  auto rate = std::uint32_t{0};
  const auto parsed =
      std::from_chars(value.data(), value.data() + value.size(), rate);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != value.data() + value.size() ||
      !isSelectableSampleRate(rate)) {
    return false;
  }
  policy.mode = SampleRateMode::fixed;
  policy.fixedRate = rate;
  return true;
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
    const StartupConfigLoadResult &configured) {
  auto contents = std::string("# Managed by PipeTune.\n");
  if (configured.presetFound) {
    const auto validation = validatePresetPath(configured.presetPath);
    if (!validation.empty()) {
      return validation;
    }
    contents += std::string(kPresetAssignment) +
                encodeConfigValue(configured.presetPath.string()) + "\n";
  }
  if (configured.preferredOutputFound) {
    const auto validation =
        validatePreferredOutput(configured.preferredOutput);
    if (!validation.empty()) {
      return validation;
    }
    contents += std::string(kTargetAssignment) +
                encodeConfigValue(configured.preferredOutput) + "\n";
  }
  contents += std::string(kDspBackendAssignment) +
              std::string(dspBackendName(configured.dspBackend)) + "\n";
  if (!sampleRatePolicyIsValid(configured.ratePolicy)) {
    return "sample-rate policy is invalid";
  }
  contents += std::string(kRateAssignment);
  if (configured.ratePolicy.mode == SampleRateMode::maximum) {
    contents += "max\n";
  } else {
    contents += std::to_string(configured.ratePolicy.fixedRate) + "\n";
  }
  contents += std::string(kRateEnforcementAssignment) +
              std::string(sampleRateEnforcementName(
                  configured.ratePolicy.enforcement)) +
              "\n";

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
  const auto configured = loadStartupConfig(configPath);
  return {.found = configured.presetFound,
          .presetPath = configured.presetPath,
          .error = configured.error};
}

StartupConfigLoadResult
loadStartupConfig(const std::filesystem::path &configPath) {
  auto contents = std::string{};
  auto configFound = false;
  const auto readError = readConfig(configPath, contents, configFound);
  if (!readError.empty()) {
    return {.presetFound = false,
            .presetPath = {},
            .preferredOutputFound = false,
            .preferredOutput = {},
            .ratePolicy = defaultSampleRatePolicy(),
            .error = readError};
  }
  if (!configFound) {
    return {.presetFound = false,
            .presetPath = {},
            .preferredOutputFound = false,
            .preferredOutput = {},
            .ratePolicy = defaultSampleRatePolicy(),
            .error = {}};
  }

  auto presetFound = false;
  auto presetPath = std::filesystem::path{};
  auto preferredOutputFound = false;
  auto preferredOutput = std::string{};
  auto dspBackendFound = false;
  auto dspBackend = DspBackendKind::scalar;
  auto rateFound = false;
  auto enforcementFound = false;
  auto ratePolicy = defaultSampleRatePolicy();
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
      if (line.starts_with(kPresetAssignment)) {
        if (presetFound) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "startup configuration contains duplicate "
                           "PIPETUNE_PRESET assignments"};
        }
        auto decoded = std::string{};
        if (!decodeConfigValue(line.substr(kPresetAssignment.size()),
                               decoded)) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "startup preset assignment is invalid"};
        }
        presetPath = std::filesystem::path(decoded);
        const auto validation = validatePresetPath(presetPath);
        if (!validation.empty()) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = validation};
        }
        presetFound = true;
      } else if (line.starts_with(kTargetAssignment)) {
        if (preferredOutputFound) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "startup configuration contains duplicate "
                           "PIPETUNE_TARGET assignments"};
        }
        if (!decodeConfigValue(line.substr(kTargetAssignment.size()),
                               preferredOutput)) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "preferred output assignment is invalid"};
        }
        const auto validation =
            validatePreferredOutput(preferredOutput);
        if (!validation.empty()) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = validation};
        }
        preferredOutputFound = true;
      } else if (line.starts_with(kDspBackendAssignment)) {
        if (dspBackendFound) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "startup configuration contains duplicate "
                           "PIPETUNE_DSP_BACKEND assignments"};
        }
        const auto parsed = parseDspBackendName(
            line.substr(kDspBackendAssignment.size()));
        if (!parsed.has_value()) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "DSP backend assignment is invalid"};
        }
        dspBackend = *parsed;
        dspBackendFound = true;
      } else if (line.starts_with(kRateAssignment)) {
        if (rateFound) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "startup configuration contains duplicate "
                           "PIPETUNE_RATE assignments"};
        }
        if (!parseConfiguredRate(line.substr(kRateAssignment.size()),
                                 ratePolicy)) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "sample-rate assignment is invalid"};
        }
        rateFound = true;
      } else if (line.starts_with(kRateEnforcementAssignment)) {
        if (enforcementFound) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "startup configuration contains duplicate "
                           "PIPETUNE_RATE_ENFORCEMENT assignments"};
        }
        if (!parseSampleRateEnforcement(
                line.substr(kRateEnforcementAssignment.size()),
                ratePolicy.enforcement)) {
          return {.presetFound = false,
                  .presetPath = {},
                  .preferredOutputFound = false,
                  .preferredOutput = {},
                  .ratePolicy = defaultSampleRatePolicy(),
                  .error = "sample-rate enforcement assignment is invalid"};
        }
        enforcementFound = true;
      } else {
        return {.presetFound = false,
                .presetPath = {},
                .preferredOutputFound = false,
                .preferredOutput = {},
                .ratePolicy = defaultSampleRatePolicy(),
                .error = "startup configuration contains an unsupported line"};
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return {.presetFound = presetFound,
          .presetPath = std::move(presetPath),
          .preferredOutputFound = preferredOutputFound,
          .preferredOutput = std::move(preferredOutput),
          .ratePolicy = ratePolicy,
          .dspBackend = dspBackend,
          .error = {}};
}

std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.presetFound = true;
  configured.presetPath = presetPath;
  return writeStartupConfig(configPath, configured);
}

std::string clearStartupPreset(const std::filesystem::path &configPath) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.presetFound = false;
  configured.presetPath.clear();
  return writeStartupConfig(configPath, configured);
}

std::string savePreferredOutput(const std::filesystem::path &configPath,
                                std::string_view nodeName) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.preferredOutputFound = true;
  configured.preferredOutput = nodeName;
  return writeStartupConfig(configPath, configured);
}

std::string clearPreferredOutput(const std::filesystem::path &configPath) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.preferredOutputFound = false;
  configured.preferredOutput.clear();
  return writeStartupConfig(configPath, configured);
}

std::string saveSampleRatePolicy(const std::filesystem::path &configPath,
                                 const SampleRatePolicy &policy) {
  if (!sampleRatePolicyIsValid(policy)) {
    return "sample-rate policy is invalid";
  }
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.ratePolicy = policy;
  return writeStartupConfig(configPath, configured);
}

std::string saveDspBackendKind(const std::filesystem::path &configPath,
                               DspBackendKind kind) {
  if (kind != DspBackendKind::scalar && kind != DspBackendKind::simd) {
    return "DSP backend is invalid";
  }
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.dspBackend = kind;
  return writeStartupConfig(configPath, configured);
}

std::string resetStartupConfig(const std::filesystem::path &configPath) {
  return writeStartupConfig(
      configPath,
      {.presetFound = false,
       .presetPath = {},
       .preferredOutputFound = false,
       .preferredOutput = {},
       .ratePolicy = defaultSampleRatePolicy(),
       .dspBackend = DspBackendKind::scalar,
       .error = {}});
}

} // namespace pipetune
