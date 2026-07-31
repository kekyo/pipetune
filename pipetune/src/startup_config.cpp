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
constexpr auto kDspSimdVariantAssignment =
    std::string_view{"PIPETUNE_DSP_SIMD_VARIANT="};
constexpr auto kDspIdlePolicyAssignment =
    std::string_view{"PIPETUNE_DSP_IDLE_POLICY="};
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

std::string saveStartupConfig(const std::filesystem::path &configPath,
                              const StartupConfig &config) {
  auto contents = std::string("# Managed by PipeTune.\n");
  if (config.presetFound) {
    const auto validation = validatePresetPath(config.presetPath);
    if (!validation.empty()) {
      return validation;
    }
    contents += std::string(kPresetAssignment) +
                encodeConfigValue(config.presetPath.string()) + "\n";
  }
  if (config.preferredOutputFound) {
    const auto validation =
        validatePreferredOutput(config.preferredOutput);
    if (!validation.empty()) {
      return validation;
    }
    contents += std::string(kTargetAssignment) +
                encodeConfigValue(config.preferredOutput) + "\n";
  }
  const auto backendName = dspBackendName(config.dspBackend);
  if (backendName.empty()) {
    return "DSP backend is invalid";
  }
  contents += std::string(kDspBackendAssignment) +
              std::string(backendName) + "\n";
  const auto simdVariantName = dspSimdVariantName(config.dspSimdVariant);
  if (simdVariantName.empty()) {
    return "DSP SIMD variant is invalid";
  }
  contents += std::string(kDspSimdVariantAssignment) +
              std::string(simdVariantName) + "\n";
  const auto idlePolicyName = dspIdlePolicyName(config.dspIdlePolicy);
  if (idlePolicyName.empty()) {
    return "DSP idle policy is invalid";
  }
  contents += std::string(kDspIdlePolicyAssignment) +
              std::string(idlePolicyName) + "\n";
  if (!sampleRatePolicyIsValid(config.ratePolicy)) {
    return "sample-rate policy is invalid";
  }
  contents += std::string(kRateAssignment);
  if (config.ratePolicy.mode == SampleRateMode::maximum) {
    contents += "max\n";
  } else {
    contents += std::to_string(config.ratePolicy.fixedRate) + "\n";
  }
  contents += std::string(kRateEnforcementAssignment) +
              std::string(sampleRateEnforcementName(
                  config.ratePolicy.enforcement)) +
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
  return {.found = configured.config.presetFound,
          .presetPath = configured.config.presetPath,
          .error = configured.error};
}

StartupConfigLoadResult
loadStartupConfig(const std::filesystem::path &configPath) {
  const auto fail = [](std::string error) {
    return StartupConfigLoadResult{.config = {}, .error = std::move(error)};
  };
  auto contents = std::string{};
  auto configFound = false;
  const auto readError = readConfig(configPath, contents, configFound);
  if (!readError.empty()) {
    return fail(readError);
  }
  if (!configFound) {
    return {.config = {}, .error = {}};
  }

  auto config = StartupConfig{};
  auto dspBackendFound = false;
  auto dspSimdVariantFound = false;
  auto dspIdlePolicyFound = false;
  auto rateFound = false;
  auto enforcementFound = false;
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
        if (config.presetFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_PRESET assignments");
        }
        auto decoded = std::string{};
        if (!decodeConfigValue(line.substr(kPresetAssignment.size()),
                               decoded)) {
          return fail("startup preset assignment is invalid");
        }
        config.presetPath = std::filesystem::path(decoded);
        const auto validation = validatePresetPath(config.presetPath);
        if (!validation.empty()) {
          return fail(validation);
        }
        config.presetFound = true;
      } else if (line.starts_with(kTargetAssignment)) {
        if (config.preferredOutputFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_TARGET assignments");
        }
        if (!decodeConfigValue(line.substr(kTargetAssignment.size()),
                               config.preferredOutput)) {
          return fail("preferred output assignment is invalid");
        }
        const auto validation =
            validatePreferredOutput(config.preferredOutput);
        if (!validation.empty()) {
          return fail(validation);
        }
        config.preferredOutputFound = true;
      } else if (line.starts_with(kDspBackendAssignment)) {
        if (dspBackendFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_DSP_BACKEND assignments");
        }
        const auto parsed = parseDspBackendName(
            line.substr(kDspBackendAssignment.size()));
        if (!parsed.has_value()) {
          return fail("DSP backend assignment is invalid");
        }
        config.dspBackend = *parsed;
        dspBackendFound = true;
      } else if (line.starts_with(kDspSimdVariantAssignment)) {
        if (dspSimdVariantFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_DSP_SIMD_VARIANT assignments");
        }
        const auto parsed = parseDspSimdVariantName(
            line.substr(kDspSimdVariantAssignment.size()));
        if (!parsed.has_value()) {
          return fail("DSP SIMD variant assignment is invalid");
        }
        config.dspSimdVariant = *parsed;
        dspSimdVariantFound = true;
      } else if (line.starts_with(kDspIdlePolicyAssignment)) {
        if (dspIdlePolicyFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_DSP_IDLE_POLICY assignments");
        }
        const auto parsed = parseDspIdlePolicyName(
            line.substr(kDspIdlePolicyAssignment.size()));
        if (!parsed.has_value()) {
          return fail("DSP idle policy assignment is invalid");
        }
        config.dspIdlePolicy = *parsed;
        dspIdlePolicyFound = true;
      } else if (line.starts_with(kRateAssignment)) {
        if (rateFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_RATE assignments");
        }
        if (!parseConfiguredRate(line.substr(kRateAssignment.size()),
                                 config.ratePolicy)) {
          return fail("sample-rate assignment is invalid");
        }
        rateFound = true;
      } else if (line.starts_with(kRateEnforcementAssignment)) {
        if (enforcementFound) {
          return fail("startup configuration contains duplicate "
                      "PIPETUNE_RATE_ENFORCEMENT assignments");
        }
        if (!parseSampleRateEnforcement(
                line.substr(kRateEnforcementAssignment.size()),
                config.ratePolicy.enforcement)) {
          return fail("sample-rate enforcement assignment is invalid");
        }
        enforcementFound = true;
      } else {
        return fail("startup configuration contains an unsupported line");
      }
    }
    if (end == std::string::npos) {
      break;
    }
    offset = end + 1;
  }
  return {.config = std::move(config), .error = {}};
}

std::string saveStartupPreset(const std::filesystem::path &configPath,
                              const std::filesystem::path &presetPath) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.config.presetFound = true;
  configured.config.presetPath = presetPath;
  return saveStartupConfig(configPath, configured.config);
}

std::string clearStartupPreset(const std::filesystem::path &configPath) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.config.presetFound = false;
  configured.config.presetPath.clear();
  return saveStartupConfig(configPath, configured.config);
}

std::string savePreferredOutput(const std::filesystem::path &configPath,
                                std::string_view nodeName) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.config.preferredOutputFound = true;
  configured.config.preferredOutput = nodeName;
  return saveStartupConfig(configPath, configured.config);
}

std::string clearPreferredOutput(const std::filesystem::path &configPath) {
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.config.preferredOutputFound = false;
  configured.config.preferredOutput.clear();
  return saveStartupConfig(configPath, configured.config);
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
  configured.config.ratePolicy = policy;
  return saveStartupConfig(configPath, configured.config);
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
  configured.config.dspBackend = kind;
  return saveStartupConfig(configPath, configured.config);
}

std::string saveDspBackendSelection(
    const std::filesystem::path &configPath, DspBackendKind kind,
    DspSimdVariant simdVariant) {
  if (kind != DspBackendKind::scalar && kind != DspBackendKind::simd) {
    return "DSP backend is invalid";
  }
  if (dspSimdVariantName(simdVariant).empty()) {
    return "DSP SIMD variant is invalid";
  }
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.config.dspBackend = kind;
  configured.config.dspSimdVariant = simdVariant;
  return saveStartupConfig(configPath, configured.config);
}

std::string saveDspIdlePolicy(const std::filesystem::path &configPath,
                              DspIdlePolicy policy) {
  if (dspIdlePolicyName(policy).empty()) {
    return "DSP idle policy is invalid";
  }
  auto configured = loadStartupConfig(configPath);
  if (!configured.error.empty()) {
    return configured.error;
  }
  configured.config.dspIdlePolicy = policy;
  return saveStartupConfig(configPath, configured.config);
}

std::string resetStartupConfig(const std::filesystem::path &configPath) {
  return saveStartupConfig(
      configPath,
      {.presetFound = false,
       .presetPath = {},
       .preferredOutputFound = false,
       .preferredOutput = {},
       .ratePolicy = defaultSampleRatePolicy(),
       .dspBackend = DspBackendKind::scalar,
       .dspSimdVariant = DspSimdVariant::automatic,
       .dspIdlePolicy = defaultDspIdlePolicy()});
}

} // namespace pipetune
