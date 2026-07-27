#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static void writeConfig(const std::filesystem::path &path,
                        std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream(path, std::ios::binary);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static bool testPathResolution() {
  const auto explicitPath =
      pipetune::resolveStartupConfigPath("/tmp/xdg-config", "/tmp/home");
  const auto fallbackPath =
      pipetune::resolveStartupConfigPath({}, "/tmp/home");
  const auto unavailable = pipetune::resolveStartupConfigPath({}, {});
  return check(explicitPath.error.empty() &&
                   explicitPath.path ==
                       "/tmp/xdg-config/pipetune/environment",
               "XDG startup config path differs") &&
         check(fallbackPath.error.empty() &&
                   fallbackPath.path ==
                       "/tmp/home/.config/pipetune/environment",
               "fallback startup config path differs") &&
         check(!unavailable.error.empty(),
               "missing XDG_CONFIG_HOME and HOME must be rejected");
}

static bool testPrivateRoundTrip(const std::filesystem::path &configPath) {
  const auto presetPath =
      std::filesystem::path("/tmp/Music \"wide\" \\\\ room.effetune_preset");
  const auto saved = pipetune::saveStartupPreset(configPath, presetPath);
  if (!check(saved.empty(), saved)) {
    return false;
  }

  struct stat fileStatus {};
  struct stat directoryStatus {};
  const auto loaded = pipetune::loadStartupPreset(configPath);
  if (!check(loaded.error.empty(), loaded.error) ||
      !check(loaded.found, "saved startup preset was not found") ||
      !check(loaded.presetPath == presetPath,
             "startup preset did not round-trip") ||
      !check(stat(configPath.c_str(), &fileStatus) == 0 &&
                 (fileStatus.st_mode & 0777) == 0600,
             "startup configuration must be private") ||
      !check(stat(configPath.parent_path().c_str(), &directoryStatus) == 0 &&
                 (directoryStatus.st_mode & 0777) == 0700,
             "startup configuration directory must be private")) {
    return false;
  }

  const auto rejected =
      pipetune::saveStartupPreset(configPath, "relative.effetune_preset");
  const auto preserved = pipetune::loadStartupPreset(configPath);
  if (!check(!rejected.empty(), "relative presets must be rejected") ||
      !check(preserved.error.empty() && preserved.found &&
                 preserved.presetPath == presetPath,
             "a rejected save must preserve the previous startup preset")) {
    return false;
  }

  const auto cleared = pipetune::clearStartupPreset(configPath);
  const auto bypass = pipetune::loadStartupPreset(configPath);
  return check(cleared.empty(), cleared) &&
         check(bypass.error.empty() && !bypass.found,
               "cleared configuration must select startup bypass") &&
         check(std::filesystem::exists(configPath),
               "clearing a preset must leave an explicit managed configuration");
}

static bool testAcceptedInputForms(const std::filesystem::path &configPath) {
  writeConfig(configPath,
              "# Existing package configuration\n"
              "PIPETUNE_PRESET=/tmp/plain.effetune_preset\n");
  const auto unquoted = pipetune::loadStartupPreset(configPath);
  if (!check(unquoted.error.empty() && unquoted.found &&
                 unquoted.presetPath == "/tmp/plain.effetune_preset",
             "existing unquoted preset assignments must remain readable")) {
    return false;
  }

  writeConfig(configPath,
              "PIPETUNE_PRESET=\"/tmp/quoted \\\"room\\\" "
              "\\\\mix.effetune_preset\"\n");
  const auto quoted = pipetune::loadStartupPreset(configPath);
  if (!check(quoted.error.empty() && quoted.found &&
                 quoted.presetPath ==
                     "/tmp/quoted \"room\" \\mix.effetune_preset",
             "quoted preset escapes must be decoded")) {
    return false;
  }

  writeConfig(configPath, "# Managed by PipeTune.\n");
  const auto absent = pipetune::loadStartupPreset(configPath);
  std::filesystem::remove(configPath);
  const auto missing = pipetune::loadStartupPreset(configPath);
  return check(absent.error.empty() && !absent.found,
               "a configuration without PIPETUNE_PRESET must select bypass") &&
         check(missing.error.empty() && !missing.found,
               "a missing configuration must select bypass");
}

static bool testRejectedInputForms(const std::filesystem::path &configPath) {
  writeConfig(configPath,
              "PIPETUNE_PRESET=/tmp/one.effetune_preset\n"
              "PIPETUNE_PRESET=/tmp/two.effetune_preset\n");
  const auto duplicate = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath,
              "PIPETUNE_PRESET=relative.effetune_preset\n");
  const auto relative = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath,
              "PIPETUNE_PRESET=\"/tmp/broken\\q.effetune_preset\"\n");
  const auto malformed = pipetune::loadStartupPreset(configPath);

  writeConfig(configPath,
              std::string(64 * 1024 + 1, '#'));
  const auto oversized = pipetune::loadStartupPreset(configPath);

  return check(!duplicate.error.empty(),
               "duplicate preset assignments must be rejected") &&
         check(!relative.error.empty(),
               "relative preset assignments must be rejected") &&
         check(!malformed.error.empty(),
               "malformed quoted assignments must be rejected") &&
         check(!oversized.error.empty(),
               "startup configurations larger than 64 KiB must be rejected");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-config-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto configPath = directory / "pipetune" / "environment";
  const auto passed =
      testPathResolution() && testPrivateRoundTrip(configPath) &&
      testAcceptedInputForms(configPath) && testRejectedInputForms(configPath);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
