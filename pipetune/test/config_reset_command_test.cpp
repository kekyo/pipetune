#include "config_reset_command.h"

#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

struct ProcessProbe {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  pipetune::ProcessWaitMode mode = pipetune::ProcessWaitMode::detached;
  std::size_t calls = 0;
  pipetune::ProcessResult result = {
      .started = true, .exitCode = 0, .error = {}};
};

static pipetune::ProcessResult
probeProcess(const std::filesystem::path &executable,
             std::span<const std::string> arguments,
             pipetune::ProcessWaitMode mode, void *userData) {
  auto &probe = *static_cast<ProcessProbe *>(userData);
  probe.executable = executable;
  probe.arguments.assign(arguments.begin(), arguments.end());
  probe.mode = mode;
  ++probe.calls;
  return probe.result;
}

static void writeLegacyConfig(const std::filesystem::path &path) {
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream(path, std::ios::binary);
  stream << "# Old PipeTune configuration\n"
            "PIPETUNE_PRESET=\"/tmp/old.effetune_preset\"\n"
            "PIPETUNE_TARGET=\"alsa_output.old\"\n"
            "PIPETUNE_RATE_MODE=\"fixed\"\n"
            "PIPETUNE_FIXED_RATE=\"96000\"\n"
            "PIPETUNE_MAX_RATE=\"192000\"\n"
            "PIPETUNE_OVERSAMPLING=\"1\"\n";
}

static bool defaultsWereStored(
    const std::filesystem::path &configPath) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(!loaded.config.presetFound &&
                   loaded.config.presetPath.empty(),
               "reset configuration must select startup bypass") &&
         check(!loaded.config.preferredOutputFound &&
                   loaded.config.preferredOutput.empty(),
               "reset configuration must follow the system output") &&
         check(loaded.config.ratePolicy ==
                   pipetune::defaultSampleRatePolicy(),
               "reset configuration must select Max and suggest") &&
         check(loaded.config.dspBackend ==
                       pipetune::DspBackendKind::scalar &&
                   loaded.config.dspSimdVariant ==
                       pipetune::DspSimdVariant::automatic,
               "reset configuration must select scalar with automatic SIMD");
}

static bool testConfirmationParsing() {
  return check(pipetune::configurationResetIsConfirmed("y"),
               "lowercase y must confirm reset") &&
         check(pipetune::configurationResetIsConfirmed(" YES "),
               "trimmed mixed-case yes must confirm reset") &&
         check(!pipetune::configurationResetIsConfirmed(""),
               "empty confirmation must cancel reset") &&
         check(!pipetune::configurationResetIsConfirmed("n"),
               "negative confirmation must cancel reset") &&
         check(!pipetune::configurationResetIsConfirmed("yes please"),
               "additional confirmation text must cancel reset");
}

static bool testLegacyConfigurationIsReset(
    const std::filesystem::path &configPath) {
  writeLegacyConfig(configPath);
  auto probe = ProcessProbe{};
  const auto result = pipetune::executeConfigurationReset(
      {.configPath = configPath,
       .systemctlExecutable = "/usr/bin/systemctl",
       .processRunner = probeProcess,
       .processUserData = &probe});
  struct stat fileStatus {};
  struct stat directoryStatus {};
  if (!check(result.success, result.error) ||
      !check(result.configurationReset,
             "successful operation must report the persisted reset") ||
      !check(probe.calls == 1,
             "successful reset must request one service restart") ||
      !check(probe.executable == "/usr/bin/systemctl",
             "configuration reset systemctl path differs") ||
      !check(probe.arguments ==
                 std::vector<std::string>{
                     "--user", "try-restart", "pipetune.service"},
             "configuration reset systemctl arguments differ") ||
      !check(probe.mode == pipetune::ProcessWaitMode::wait,
             "configuration reset must await service restart") ||
      !defaultsWereStored(configPath) ||
      !check(stat(configPath.c_str(), &fileStatus) == 0 &&
                 (fileStatus.st_mode & 0777) == 0600,
             "reset configuration must be private") ||
      !check(stat(configPath.parent_path().c_str(),
                  &directoryStatus) == 0 &&
                 (directoryStatus.st_mode & 0777) == 0700,
             "reset configuration directory must be private")) {
    return false;
  }

  const auto repeated = pipetune::executeConfigurationReset(
      {.configPath = configPath,
       .systemctlExecutable = "/usr/bin/systemctl",
       .processRunner = probeProcess,
       .processUserData = &probe});
  return check(repeated.success, repeated.error) &&
         check(probe.calls == 2,
               "repeated reset must remain safe and request live apply") &&
         defaultsWereStored(configPath);
}

static bool testPersistenceFailureSkipsRestart() {
  auto probe = ProcessProbe{};
  const auto result = pipetune::executeConfigurationReset(
      {.configPath = {},
       .systemctlExecutable = "/usr/bin/systemctl",
       .processRunner = probeProcess,
       .processUserData = &probe});
  return check(!result.success,
               "invalid configuration path must fail reset") &&
         check(!result.configurationReset,
               "failed persistence must not report a reset") &&
         check(probe.calls == 0,
               "failed persistence must not restart the service");
}

static bool testRestartFailureKeepsReset(
    const std::filesystem::path &configPath) {
  writeLegacyConfig(configPath);
  auto probe = ProcessProbe{};
  probe.result = {.started = true, .exitCode = 1, .error = {}};
  const auto result = pipetune::executeConfigurationReset(
      {.configPath = configPath,
       .systemctlExecutable = "/usr/bin/systemctl",
       .processRunner = probeProcess,
       .processUserData = &probe});
  return check(!result.success,
               "service restart failure must report partial failure") &&
         check(result.configurationReset,
               "restart failure must retain the persisted reset") &&
         check(result.error.find("configuration was reset") !=
                   std::string::npos,
               "partial failure must explain that persistence succeeded") &&
         check(probe.calls == 1,
               "restart failure must not be retried implicitly") &&
         defaultsWereStored(configPath);
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-config-reset-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto firstConfig = directory / "first" / "environment";
  const auto secondConfig = directory / "second" / "environment";
  const auto passed =
      testConfirmationParsing() &&
      testLegacyConfigurationIsReset(firstConfig) &&
      testPersistenceFailureSkipsRestart() &&
      testRestartFailureKeepsReset(secondConfig);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
