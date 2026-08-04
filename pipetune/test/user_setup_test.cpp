#include "user_setup.h"

#include "autostart_override.h"
#include "pipetune/startup_config.h"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

struct Invocation {
  std::filesystem::path executable;
  std::vector<std::string> arguments;
  pipetune::ProcessWaitMode mode;
};

struct FakeProcessRunner {
  std::vector<pipetune::ProcessResult> results;
  std::vector<Invocation> invocations;
};

struct FakeIntegrationProbe {
  std::size_t calls = 0;
  std::string error;
};

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ProcessResult fakeRunProcess(
    const std::filesystem::path &executable,
    std::span<const std::string> arguments,
    pipetune::ProcessWaitMode mode, void *userData) {
  auto &runner = *static_cast<FakeProcessRunner *>(userData);
  runner.invocations.push_back(
      {.executable = executable,
       .arguments = std::vector<std::string>(arguments.begin(),
                                             arguments.end()),
       .mode = mode});
  const auto index = runner.invocations.size() - 1;
  if (index < runner.results.size()) {
    return runner.results[index];
  }
  return {.started = true, .exitCode = 0, .error = {}};
}

static std::string fakeIntegrationProbe(void *userData) {
  auto &probe = *static_cast<FakeIntegrationProbe *>(userData);
  ++probe.calls;
  return probe.error;
}

static pipetune::UserManagementPaths makePaths(
    const std::filesystem::path &directory) {
  return {
      .configPath = directory / "config" / "pipetune" / "environment",
      .legacyConfigPath =
          directory / "config" / "pipetune" / "environment.gtk",
      .autostartPath =
          directory / "config" / "autostart" /
          "net.kekyo.pipetune-gtk.desktop",
      .autostartBackupPath =
          directory / "config" / "autostart" /
          "net.kekyo.pipetune-gtk.desktop.pipetune-backup",
      .systemctlExecutable = "/test/systemctl",
      .gtkExecutable = "/test/pipetune-gtk",
  };
}

static pipetune::ProcessResult processResult(int exitCode) {
  return {.started = true, .exitCode = exitCode, .error = {}};
}

static void writeFile(const std::filesystem::path &path,
                      std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream(path, std::ios::binary);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static std::string readFile(const std::filesystem::path &path) {
  auto stream = std::ifstream(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(stream),
                     std::istreambuf_iterator<char>());
}

static std::filesystem::path writePreset(
    const std::filesystem::path &directory, std::string_view name) {
  const auto path = directory / std::string(name);
  writeFile(path, R"json({"pipeline":[]})json");
  return path;
}

static bool invocationMatches(const Invocation &invocation,
                              std::string_view executable,
                              std::initializer_list<std::string_view> arguments,
                              pipetune::ProcessWaitMode mode) {
  if (invocation.executable != executable || invocation.mode != mode ||
      invocation.arguments.size() != arguments.size()) {
    return false;
  }
  auto index = std::size_t{0};
  for (const auto argument : arguments) {
    if (invocation.arguments[index++] != argument) {
      return false;
    }
  }
  return true;
}

static bool testSetupPreservesConfigurationAndRestoresAutostart(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "preserve");
  const auto existingPreset = writePreset(directory, "existing.effetune_preset");
  const auto saved =
      pipetune::saveStartupPreset(paths.configPath, existingPreset);
  if (!check(saved.empty(), saved)) {
    return false;
  }
  constexpr auto customOverride =
      "[Desktop Entry]\nType=Application\nX-Custom=true\n";
  writeFile(paths.autostartPath, customOverride);
  const auto masked = pipetune::maskGtkAutostart(
      paths.autostartPath, paths.autostartBackupPath);
  if (!check(masked.success, masked.error)) {
    return false;
  }

  auto runner = FakeProcessRunner{
      .results = {processResult(1), processResult(1), processResult(0),
                  processResult(0), processResult(0), processResult(0),
                  processResult(0), processResult(0)},
      .invocations = {}};
  auto integration = FakeIntegrationProbe{};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &integration});
  const auto loaded = pipetune::loadStartupPreset(paths.configPath);
  return check(result.success, result.error) &&
         check(loaded.error.empty() && loaded.found &&
                   loaded.presetPath == existingPreset,
               "setup without --preset must preserve the existing preset") &&
         check(readFile(paths.autostartPath) == customOverride &&
                   !std::filesystem::exists(paths.autostartBackupPath),
               "setup must restore a backed-up custom autostart override") &&
         check(runner.invocations.size() == 8,
               "setup process invocation count differs") &&
         check(integration.calls == 1,
               "setup must verify the WirePlumber policy handshake") &&
         check(invocationMatches(
                   runner.invocations[2], "/test/systemctl",
                   {"--user", "daemon-reload"},
                   pipetune::ProcessWaitMode::wait),
               "setup must reload the user manager first") &&
         check(invocationMatches(
                   runner.invocations[3], "/test/systemctl",
                   {"--user", "try-restart", "wireplumber.service"},
                   pipetune::ProcessWaitMode::wait),
               "setup must reload the installed WirePlumber policy") &&
         check(invocationMatches(
                   runner.invocations[4], "/test/systemctl",
                   {"--user", "enable", "pipetune.service"},
                   pipetune::ProcessWaitMode::wait),
               "setup enable invocation differs") &&
         check(invocationMatches(
                   runner.invocations[5], "/test/systemctl",
                   {"--user", "restart", "pipetune.service"},
                   pipetune::ProcessWaitMode::wait),
               "setup restart invocation differs") &&
         check(invocationMatches(
                   runner.invocations[6], "/test/systemctl",
                   {"--user", "is-active", "--quiet", "pipetune.service"},
                   pipetune::ProcessWaitMode::wait),
               "setup active verification differs") &&
         check(invocationMatches(
                   runner.invocations[7], "/test/pipetune-gtk",
                   {"--hidden"}, pipetune::ProcessWaitMode::detached),
               "setup must finish by launching GTK hidden");
}

static bool testExplicitPresetAndValidation(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "explicit");
  const auto preset = writePreset(directory, "selected.effetune_preset");
  auto runner = FakeProcessRunner{
      .results = {processResult(1), processResult(1), processResult(0),
                  processResult(0), processResult(0), processResult(0),
                  processResult(0), processResult(0)},
      .invocations = {}};
  auto integration = FakeIntegrationProbe{};
  const auto configured = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .presetSpecified = true,
       .presetPath = preset,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &integration});
  const auto loaded = pipetune::loadStartupPreset(paths.configPath);
  if (!check(configured.success, configured.error) ||
      !check(loaded.error.empty() && loaded.found &&
                 loaded.presetPath == preset,
             "setup --preset must save the validated preset")) {
    return false;
  }

  auto invalidRunner =
      FakeProcessRunner{.results = {}, .invocations = {}};
  auto invalidIntegration = FakeIntegrationProbe{};
  const auto invalid = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .presetSpecified = true,
       .presetPath = directory / "missing.effetune_preset",
       .paths = makePaths(directory / "invalid"),
       .processRunner = fakeRunProcess,
       .processUserData = &invalidRunner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &invalidIntegration});
  return check(!invalid.success,
               "an invalid explicit preset must fail setup") &&
         check(invalidRunner.invocations.empty(),
               "invalid explicit preset must fail before external changes");
}

static bool testWirePlumberPolicyReloadFailure(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "wireplumber-reload");
  auto runner = FakeProcessRunner{
      .results = {processResult(1), processResult(1), processResult(0),
                  processResult(1)},
      .invocations = {}};
  auto integration = FakeIntegrationProbe{};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &integration});
  return check(!result.success,
               "WirePlumber policy reload failure must fail setup") &&
         check(result.error ==
                   "cannot reload WirePlumber policy exited with status 1",
               "WirePlumber policy reload diagnostic differs") &&
         check(runner.invocations.size() == 4,
               "failed WirePlumber reload must stop setup") &&
         check(invocationMatches(
                   runner.invocations[3], "/test/systemctl",
                   {"--user", "try-restart", "wireplumber.service"},
                   pipetune::ProcessWaitMode::wait),
               "WirePlumber policy reload invocation differs");
}

static bool testWirePlumberPolicyHandshakeFailure(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "wireplumber-handshake");
  auto runner = FakeProcessRunner{
      .results = {processResult(1), processResult(1), processResult(0),
                  processResult(0), processResult(0), processResult(0),
                  processResult(0), processResult(0), processResult(0)},
      .invocations = {}};
  auto integration = FakeIntegrationProbe{
      .calls = 0,
      .error = "WirePlumber PipeTune policy handshake is unavailable"};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &integration});
  return check(!result.success,
               "a missing policy handshake must fail setup") &&
         check(result.error == integration.error,
               "policy handshake failure diagnostic differs") &&
         check(integration.calls == 1,
               "setup integration probe invocation differs") &&
         check(std::none_of(
                   runner.invocations.begin(), runner.invocations.end(),
                   [](const auto &invocation) {
                     return invocation.executable == "/test/pipetune-gtk";
                   }),
               "failed policy verification must not launch GTK");
}

static bool testSetupRollback(const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "rollback");
  const auto oldPreset = writePreset(directory, "old.effetune_preset");
  const auto newPreset = writePreset(directory, "new.effetune_preset");
  const auto saved = pipetune::saveStartupPreset(paths.configPath, oldPreset);
  if (!check(saved.empty(), saved)) {
    return false;
  }
  auto runner = FakeProcessRunner{
      .results = {processResult(0), processResult(0), processResult(0),
                  processResult(0), processResult(0), processResult(1),
                  processResult(0), processResult(0)},
      .invocations = {}};
  auto integration = FakeIntegrationProbe{};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .presetSpecified = true,
       .presetPath = newPreset,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &integration});
  const auto restored = pipetune::loadStartupPreset(paths.configPath);
  return check(!result.success,
               "failed service restart must fail setup") &&
         check(restored.error.empty() && restored.found &&
                   restored.presetPath == oldPreset,
               "failed setup must restore the previous configuration") &&
         check(runner.invocations.size() == 8,
               "setup rollback invocation count differs") &&
         check(invocationMatches(
                   runner.invocations[3], "/test/systemctl",
                   {"--user", "try-restart", "wireplumber.service"},
                   pipetune::ProcessWaitMode::wait),
               "setup must reload WirePlumber before starting PipeTune") &&
         check(invocationMatches(
                   runner.invocations[6], "/test/systemctl",
                   {"--user", "enable", "pipetune.service"},
                   pipetune::ProcessWaitMode::wait) &&
                   invocationMatches(
                       runner.invocations[7], "/test/systemctl",
                       {"--user", "restart", "pipetune.service"},
                       pipetune::ProcessWaitMode::wait),
               "setup rollback must restore enabled and active state");
}

static bool testUnsetupAndPurge(const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "unsetup");
  writeFile(paths.autostartPath,
            "[Desktop Entry]\nType=Application\nX-Custom=true\n");
  const auto saved = pipetune::clearStartupPreset(paths.configPath);
  if (!check(saved.empty(), saved)) {
    return false;
  }
  writeFile(paths.legacyConfigPath, "legacy");
  auto runner =
      FakeProcessRunner{.results = {processResult(0), processResult(0)},
                        .invocations = {}};
  const auto result = pipetune::executeUserUnsetup(
      {.effectiveUserId = 1000,
       .purge = true,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  return check(result.success, result.error) &&
         check(runner.invocations.size() == 2,
               "unsetup process invocation count differs") &&
         check(invocationMatches(
                   runner.invocations[0], "/test/pipetune-gtk",
                   {"--quit"}, pipetune::ProcessWaitMode::wait),
               "unsetup must ask GTK to quit before stopping the service") &&
         check(invocationMatches(
                   runner.invocations[1], "/test/systemctl",
                   {"--user", "disable", "--now", "pipetune.service"},
                   pipetune::ProcessWaitMode::wait),
               "unsetup service stop invocation differs") &&
         check(pipetune::isPipeTuneManagedAutostartMask(
                   paths.autostartPath) &&
                   std::filesystem::exists(paths.autostartBackupPath),
               "unsetup must retain its mask and custom override backup") &&
         check(!std::filesystem::exists(paths.configPath) &&
                   !std::filesystem::exists(paths.legacyConfigPath),
               "unsetup --purge must remove both app configuration files");
}

static bool testUnsetupStopFailurePreservesConfiguration(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "stop-failure");
  const auto saved = pipetune::clearStartupPreset(paths.configPath);
  if (!check(saved.empty(), saved)) {
    return false;
  }
  auto runner =
      FakeProcessRunner{.results = {processResult(0), processResult(1)},
                        .invocations = {}};
  const auto result = pipetune::executeUserUnsetup(
      {.effectiveUserId = 1000,
       .purge = true,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  return check(!result.success,
               "service stop failure must fail unsetup") &&
         check(std::filesystem::exists(paths.configPath),
               "service stop failure must prevent configuration purge") &&
         check(pipetune::isPipeTuneManagedAutostartMask(
                   paths.autostartPath),
               "failed unsetup must retain its autostart mask");
}

static bool testRootRejection(const std::filesystem::path &directory) {
  auto runner =
      FakeProcessRunner{.results = {}, .invocations = {}};
  auto integration = FakeIntegrationProbe{};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 0,
       .presetSpecified = false,
       .presetPath = {},
       .paths = makePaths(directory / "root"),
       .processRunner = fakeRunProcess,
       .processUserData = &runner,
       .integrationProbe = fakeIntegrationProbe,
       .integrationProbeUserData = &integration});
  return check(!result.success,
               "setup must reject the root user") &&
         check(runner.invocations.empty(),
               "root rejection must happen before external changes");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-user-setup-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto passed =
      testSetupPreservesConfigurationAndRestoresAutostart(directory) &&
      testExplicitPresetAndValidation(directory) &&
      testWirePlumberPolicyReloadFailure(directory) &&
      testWirePlumberPolicyHandshakeFailure(directory) &&
      testSetupRollback(directory) &&
      testUnsetupAndPurge(directory) &&
      testUnsetupStopFailurePreservesConfiguration(directory) &&
      testRootRejection(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
