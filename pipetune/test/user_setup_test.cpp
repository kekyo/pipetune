#include "user_setup.h"

#include "autostart_override.h"
#include "pipetune/startup_config.h"

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

static pipetune::UserManagementPaths makePaths(
    const std::filesystem::path &directory) {
  return {
      .configPath = directory / "config" / "pipetune" / "environment",
      .legacyConfigPath =
          directory / "config" / "pipetune" / "environment.gtk",
      .autostartPath =
          directory / "config" / "autostart" /
          "net.kekyo.pipetune_gtk.desktop",
      .autostartBackupPath =
          directory / "config" / "autostart" /
          "net.kekyo.pipetune_gtk.desktop.pipetune-backup",
      .wirePlumberPolicyPath =
          directory / "config" / "wireplumber" / "policy.lua.d" /
          "60-pipetune-filter.lua",
      .wirePlumberClientScriptPath =
          directory / "config" / "wireplumber" / "scripts" /
          "pipetune-endpoint-client.lua",
      .wirePlumberDeviceScriptPath =
          directory / "config" / "wireplumber" / "scripts" /
          "pipetune-endpoint-device.lua",
      .wirePlumber04VisibilityScriptPath =
          directory / "config" / "wireplumber" / "scripts" /
          "pipetune-node-visibility.lua",
      .wirePlumber05PolicyPath =
          directory / "config" / "wireplumber" /
          "wireplumber.conf.d" / "60-pipetune-node-visibility.conf",
      .wirePlumber05VisibilityScriptPath =
          directory / "data" / "wireplumber" / "scripts" /
          "pipetune-node-visibility.lua",
      .setupStatePath = directory / "state" / "pipetune" / "setup-state",
      .managementLockPath =
          directory / "state" / "pipetune" / "management.lock",
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

static bool audioStackRestartMatches(const Invocation &invocation) {
  return invocationMatches(
      invocation, "/test/systemctl",
      {"--user", "restart", "pipewire.service", "wireplumber.service",
       "pipewire-pulse.service"},
      pipetune::ProcessWaitMode::wait);
}

static bool testWirePlumberCompatibilityPaths(
    const std::filesystem::path &directory) {
  const auto configRoot = directory / "resolved-config";
  const auto dataRoot = directory / "resolved-data";
  const auto stateRoot = directory / "resolved-state";
  const auto resolved = pipetune::resolveUserManagementPaths(
      configRoot.string(), dataRoot.string(), stateRoot.string(), {},
      "/test/systemctl", "/test/pipetune-gtk");
  const auto home = directory / "resolved-home";
  const auto fallback = pipetune::resolveUserManagementPaths(
      {}, {}, {}, home, "/test/systemctl", "/test/pipetune-gtk");
  return check(resolved.error.empty(), resolved.error) &&
         check(fallback.error.empty(), fallback.error) &&
         check(resolved.paths.wirePlumberPolicyPath ==
                   configRoot / "wireplumber" / "policy.lua.d" /
                       "60-pipetune-filter.lua",
               "WirePlumber policy must load before 90-enable-all.lua") &&
         check(resolved.paths.wirePlumberClientScriptPath ==
                   configRoot / "wireplumber" / "scripts" /
                       "pipetune-endpoint-client.lua",
               "WirePlumber 0.4 client policy must use its script path") &&
         check(resolved.paths.wirePlumberDeviceScriptPath ==
                   configRoot / "wireplumber" / "scripts" /
                       "pipetune-endpoint-device.lua",
               "WirePlumber 0.4 device policy must use its script path") &&
         check(resolved.paths.wirePlumber04VisibilityScriptPath ==
                   configRoot / "wireplumber" / "scripts" /
                       "pipetune-node-visibility.lua",
               "WirePlumber 0.4 visibility policy must use its config path") &&
         check(resolved.paths.wirePlumber05PolicyPath ==
                   configRoot / "wireplumber" / "wireplumber.conf.d" /
                       "60-pipetune-node-visibility.conf",
               "WirePlumber 0.5 policy must use its config fragment path") &&
         check(resolved.paths.wirePlumber05VisibilityScriptPath ==
                   dataRoot / "wireplumber" / "scripts" /
                       "pipetune-node-visibility.lua",
               "WirePlumber 0.5 script must honor XDG_DATA_HOME") &&
         check(resolved.paths.setupStatePath ==
                   stateRoot / "pipetune" / "setup-state" &&
                   resolved.paths.managementLockPath ==
                       stateRoot / "pipetune" / "management.lock",
               "setup state must honor XDG_STATE_HOME") &&
         check(fallback.paths.wirePlumber05VisibilityScriptPath ==
                   home / ".local" / "share" / "wireplumber" / "scripts" /
                       "pipetune-node-visibility.lua",
               "WirePlumber 0.5 script must use the XDG data fallback") &&
         check(fallback.paths.setupStatePath ==
                   home / ".local" / "state" / "pipetune" / "setup-state",
               "setup state must use the XDG state fallback");
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
                  processResult(0)},
      .invocations = {}};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = true,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  const auto loaded = pipetune::loadStartupPreset(paths.configPath);
  const auto policy = readFile(paths.wirePlumberPolicyPath);
  const auto clientScript = readFile(paths.wirePlumberClientScriptPath);
  const auto deviceScript = readFile(paths.wirePlumberDeviceScriptPath);
  const auto visibility04 =
      readFile(paths.wirePlumber04VisibilityScriptPath);
  const auto visibility05 =
      readFile(paths.wirePlumber05VisibilityScriptPath);
  const auto visibilityConfiguration =
      readFile(paths.wirePlumber05PolicyPath);
  return check(result.success, result.error) &&
         check(std::filesystem::exists(paths.setupStatePath),
               "successful setup must record its current completion state") &&
         check(loaded.error.empty() && loaded.found &&
                   loaded.presetPath == existingPreset,
               "setup without --preset must preserve the existing preset") &&
         check(readFile(paths.autostartPath) == customOverride &&
                   !std::filesystem::exists(paths.autostartBackupPath),
               "setup must restore a backed-up custom autostart override") &&
         check(std::filesystem::exists(paths.wirePlumberPolicyPath),
               "setup must install the WirePlumber 0.4 compatibility policy") &&
         check(policy.find("endpoint.pipetune.playback") !=
                       std::string::npos &&
                   policy.find("endpoint.pipetune.capture") !=
                       std::string::npos &&
                   policy.find("pipetune-endpoint-client.lua") !=
                       std::string::npos &&
                   policy.find("pipetune-endpoint-device.lua") !=
                       std::string::npos &&
                   policy.find("pipetune-node-visibility.lua") !=
                       std::string::npos,
               "compatibility policy must configure and load the filter") &&
         check(std::filesystem::exists(paths.wirePlumberClientScriptPath),
               "setup must install the WirePlumber 0.4 client policy") &&
         check(clientScript.find("is.policy.endpoint.client.link") !=
                       std::string::npos &&
                   clientScript.find("node.link-group") != std::string::npos,
               "client policy must route applications but exclude filters") &&
         check(std::filesystem::exists(paths.wirePlumberDeviceScriptPath),
               "setup must install the WirePlumber 0.4 device policy") &&
         check(deviceScript.find("node.pipetune.target-endpoint") !=
                       std::string::npos &&
                   deviceScript.find("Stream/Output/Audio") !=
                       std::string::npos,
               "compatibility policy must connect both sides of the filter") &&
         check(std::filesystem::exists(
                   paths.wirePlumber04VisibilityScriptPath) &&
                   std::filesystem::exists(paths.wirePlumber05PolicyPath) &&
                   std::filesystem::exists(
                       paths.wirePlumber05VisibilityScriptPath),
               "setup must install visibility policy for WirePlumber 0.4 "
               "and 0.5") &&
         check(!visibility04.empty() && visibility04 == visibility05 &&
                   visibility04.find("client:update_permissions") !=
                       std::string::npos &&
                   visibility04.find("node.pipetune.internal") !=
                       std::string::npos &&
                   visibility04.find("control.endpoint.pipetune.playback") !=
                       std::string::npos &&
                   visibility04.find("control.endpoint.pipetune.capture") !=
                       std::string::npos &&
                   visibility04.find("wireplumber.daemon") !=
                       std::string::npos &&
                   visibility04.find(
                       "proxy_property(client, "
                       "\"application.process.binary\") == \"pipetune\"") !=
                       std::string::npos &&
                   visibility04.find("Stream/Output/Audio") !=
                       std::string::npos &&
                   visibility04.find("Stream/Input/Audio") !=
                       std::string::npos &&
                   visibility04.find("pipetune_audio_stream_counts") !=
                       std::string::npos &&
                   visibility04.find(
                       "client:update_permissions { [node_id] = \"all\" }") !=
                       std::string::npos &&
                   visibility04.find("client.id") != std::string::npos &&
                   visibility04.find("client[\"bound-id\"]") !=
                       std::string::npos,
               "visibility policy must grant audio stream owners routing "
               "access while hiding PipeTune nodes from other clients") &&
         check(visibilityConfiguration.find("pipetune-node-visibility.lua") !=
                       std::string::npos &&
                   visibilityConfiguration.find("required") !=
                       std::string::npos,
               "WirePlumber 0.5 must load the visibility policy component") &&
         check(runner.invocations.size() == 8,
               "setup process invocation count differs") &&
         check(invocationMatches(
                   runner.invocations[2], "/test/systemctl",
                   {"--user", "daemon-reload"},
                   pipetune::ProcessWaitMode::wait),
               "setup must reload the user manager first") &&
         check(audioStackRestartMatches(runner.invocations[3]),
               "setup must reset the audio stack after installing its "
               "policy") &&
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

static bool testSetupSkipsCurrentStateAndForceRepeats(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "conditional");
  auto initialRunner = FakeProcessRunner{
      .results = {processResult(1), processResult(1), processResult(0),
                  processResult(0), processResult(0), processResult(0),
                  processResult(0)},
      .invocations = {}};
  const auto initial = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = false,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &initialRunner});
  if (!check(initial.success, initial.error) ||
      !check(std::filesystem::exists(paths.setupStatePath),
             "initial setup must persist its completion state")) {
    return false;
  }

  auto currentRunner = FakeProcessRunner{
      .results = {processResult(0), processResult(0)}, .invocations = {}};
  const auto current = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = false,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &currentRunner});
  if (!check(current.success, current.error) ||
      !check(currentRunner.invocations.size() == 2,
             "current setup must stop after enablement and activity probes")) {
    return false;
  }

  writeFile(paths.wirePlumberPolicyPath, "outdated policy");
  auto repairRunner = FakeProcessRunner{
      .results = {processResult(0), processResult(0), processResult(0),
                  processResult(0), processResult(0), processResult(0),
                  processResult(0)},
      .invocations = {}};
  const auto repaired = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = false,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &repairRunner});
  if (!check(repaired.success, repaired.error) ||
      !check(repairRunner.invocations.size() == 7 &&
                 audioStackRestartMatches(repairRunner.invocations[3]),
             "outdated managed policy must trigger setup and audio restart")) {
    return false;
  }

  auto forcedRunner = FakeProcessRunner{
      .results = {processResult(0), processResult(0), processResult(0),
                  processResult(0), processResult(0), processResult(0)},
      .invocations = {}};
  const auto forced = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = true,
       .launchGtk = false,
       .presetSpecified = false,
       .presetPath = {},
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &forcedRunner});
  return check(forced.success, forced.error) &&
         check(forcedRunner.invocations.size() == 6,
               "forced setup must repeat service setup without relaunching GTK") &&
         check(invocationMatches(
                   forcedRunner.invocations[2], "/test/systemctl",
                   {"--user", "daemon-reload"},
                   pipetune::ProcessWaitMode::wait),
               "forced setup must perform the existing setup workflow");
}

static bool testExplicitPresetAndValidation(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "explicit");
  const auto preset = writePreset(directory, "selected.effetune_preset");
  auto runner = FakeProcessRunner{
      .results = {processResult(1), processResult(1), processResult(0),
                  processResult(0), processResult(0), processResult(0),
                  processResult(0)},
      .invocations = {}};
  const auto configured = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = true,
       .presetSpecified = true,
       .presetPath = preset,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  const auto loaded = pipetune::loadStartupPreset(paths.configPath);
  if (!check(configured.success, configured.error) ||
      !check(loaded.error.empty() && loaded.found &&
                 loaded.presetPath == preset,
             "setup --preset must save the validated preset")) {
    return false;
  }

  auto invalidRunner =
      FakeProcessRunner{.results = {}, .invocations = {}};
  const auto invalid = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = true,
       .presetSpecified = true,
       .presetPath = directory / "missing.effetune_preset",
       .paths = makePaths(directory / "invalid"),
       .processRunner = fakeRunProcess,
       .processUserData = &invalidRunner});
  return check(!invalid.success,
               "an invalid explicit preset must fail setup") &&
         check(invalidRunner.invocations.empty(),
               "invalid explicit preset must fail before external changes");
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
                  processResult(0), processResult(0), processResult(0)},
      .invocations = {}};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 1000,
       .force = false,
       .launchGtk = true,
       .presetSpecified = true,
       .presetPath = newPreset,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  const auto restored = pipetune::loadStartupPreset(paths.configPath);
  return check(!result.success,
               "failed service restart must fail setup") &&
         check(restored.error.empty() && restored.found &&
                   restored.presetPath == oldPreset,
               "failed setup must restore the previous configuration") &&
         check(!std::filesystem::exists(paths.wirePlumberPolicyPath),
               "failed setup must restore the previous WirePlumber policy") &&
         check(!std::filesystem::exists(paths.wirePlumberClientScriptPath),
               "failed setup must restore the previous client policy") &&
         check(!std::filesystem::exists(paths.wirePlumberDeviceScriptPath),
               "failed setup must restore the previous device policy") &&
         check(!std::filesystem::exists(
                   paths.wirePlumber04VisibilityScriptPath) &&
                   !std::filesystem::exists(paths.wirePlumber05PolicyPath) &&
                   !std::filesystem::exists(
                       paths.wirePlumber05VisibilityScriptPath),
               "failed setup must restore all visibility policy files") &&
         check(!std::filesystem::exists(paths.setupStatePath),
               "failed setup must not retain its completion state") &&
         check(runner.invocations.size() == 9,
               "setup rollback invocation count differs") &&
         check(audioStackRestartMatches(runner.invocations[6]) &&
                   invocationMatches(
                       runner.invocations[7], "/test/systemctl",
                       {"--user", "enable", "pipetune.service"},
                       pipetune::ProcessWaitMode::wait) &&
                   invocationMatches(
                       runner.invocations[8], "/test/systemctl",
                       {"--user", "restart", "pipetune.service"},
                       pipetune::ProcessWaitMode::wait),
               "setup rollback must restore the audio stack and service "
               "state");
}

static bool testUnsetupAndPurge(const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "unsetup");
  writeFile(paths.wirePlumberPolicyPath, "managed policy");
  writeFile(paths.wirePlumberClientScriptPath, "managed client script");
  writeFile(paths.wirePlumberDeviceScriptPath, "managed device script");
  writeFile(paths.wirePlumber04VisibilityScriptPath,
            "managed 0.4 visibility script");
  writeFile(paths.wirePlumber05PolicyPath, "managed 0.5 policy");
  writeFile(paths.wirePlumber05VisibilityScriptPath,
            "managed 0.5 visibility script");
  writeFile(paths.autostartPath,
            "[Desktop Entry]\nType=Application\nX-Custom=true\n");
  const auto saved = pipetune::clearStartupPreset(paths.configPath);
  if (!check(saved.empty(), saved)) {
    return false;
  }
  writeFile(paths.setupStatePath, "current setup state");
  writeFile(paths.legacyConfigPath, "legacy");
  auto runner =
      FakeProcessRunner{.results = {processResult(0), processResult(0),
                                    processResult(0)},
                        .invocations = {}};
  const auto result = pipetune::executeUserUnsetup(
      {.effectiveUserId = 1000,
       .purge = true,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  return check(result.success, result.error) &&
         check(runner.invocations.size() == 3,
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
         check(audioStackRestartMatches(runner.invocations[2]),
               "unsetup must reset the audio stack after removing its "
               "policy") &&
         check(!std::filesystem::exists(paths.wirePlumberPolicyPath),
               "unsetup must remove the WirePlumber 0.4 compatibility policy") &&
         check(!std::filesystem::exists(paths.wirePlumberClientScriptPath),
               "unsetup must remove the WirePlumber 0.4 client policy") &&
         check(!std::filesystem::exists(paths.wirePlumberDeviceScriptPath),
               "unsetup must remove the WirePlumber 0.4 device policy") &&
         check(!std::filesystem::exists(
                   paths.wirePlumber04VisibilityScriptPath) &&
                   !std::filesystem::exists(paths.wirePlumber05PolicyPath) &&
                   !std::filesystem::exists(
                       paths.wirePlumber05VisibilityScriptPath),
               "unsetup must remove all WirePlumber visibility policy files") &&
         check(pipetune::isPipeTuneManagedAutostartMask(
                   paths.autostartPath) &&
                   std::filesystem::exists(paths.autostartBackupPath),
               "unsetup must retain its mask and custom override backup") &&
         check(!std::filesystem::exists(paths.configPath) &&
                   !std::filesystem::exists(paths.legacyConfigPath),
               "unsetup --purge must remove both app configuration files") &&
         check(!std::filesystem::exists(paths.setupStatePath),
               "unsetup must remove the setup completion state");
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

static bool testUnsetupRestartFailureRestoresPolicies(
    const std::filesystem::path &directory) {
  const auto paths = makePaths(directory / "unsetup-rollback");
  writeFile(paths.wirePlumberPolicyPath, "policy");
  writeFile(paths.wirePlumberClientScriptPath, "client");
  writeFile(paths.wirePlumberDeviceScriptPath, "device");
  writeFile(paths.wirePlumber04VisibilityScriptPath, "visibility 0.4");
  writeFile(paths.wirePlumber05PolicyPath, "configuration 0.5");
  writeFile(paths.wirePlumber05VisibilityScriptPath, "visibility 0.5");
  auto runner =
      FakeProcessRunner{.results = {processResult(0), processResult(0),
                                    processResult(1), processResult(0)},
                        .invocations = {}};
  const auto result = pipetune::executeUserUnsetup(
      {.effectiveUserId = 1000,
       .purge = false,
       .paths = paths,
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
  return check(!result.success,
               "audio stack restart failure must fail unsetup") &&
         check(readFile(paths.wirePlumberPolicyPath) == "policy" &&
                   readFile(paths.wirePlumberClientScriptPath) == "client" &&
                   readFile(paths.wirePlumberDeviceScriptPath) == "device" &&
                   readFile(paths.wirePlumber04VisibilityScriptPath) ==
                       "visibility 0.4" &&
                   readFile(paths.wirePlumber05PolicyPath) ==
                       "configuration 0.5" &&
                   readFile(paths.wirePlumber05VisibilityScriptPath) ==
                       "visibility 0.5",
               "failed unsetup must restore every WirePlumber policy file") &&
         check(runner.invocations.size() == 4,
               "unsetup rollback invocation count differs") &&
         check(audioStackRestartMatches(runner.invocations[2]) &&
                   audioStackRestartMatches(runner.invocations[3]),
               "unsetup rollback must reactivate restored policies with a "
               "second audio stack reset");
}

static bool testRootRejection(const std::filesystem::path &directory) {
  auto runner =
      FakeProcessRunner{.results = {}, .invocations = {}};
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = 0,
       .force = false,
       .launchGtk = true,
       .presetSpecified = false,
       .presetPath = {},
       .paths = makePaths(directory / "root"),
       .processRunner = fakeRunProcess,
       .processUserData = &runner});
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
  auto passed = true;
  passed = testWirePlumberCompatibilityPaths(directory) && passed;
  passed =
      testSetupPreservesConfigurationAndRestoresAutostart(directory) && passed;
  passed = testSetupSkipsCurrentStateAndForceRepeats(directory) && passed;
  passed = testExplicitPresetAndValidation(directory) && passed;
  passed = testSetupRollback(directory) && passed;
  passed = testUnsetupAndPurge(directory) && passed;
  passed = testUnsetupStopFailurePreservesConfiguration(directory) && passed;
  passed = testUnsetupRestartFailureRestoresPolicies(directory) && passed;
  passed = testRootRejection(directory) && passed;
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
