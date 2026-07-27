#include "user_setup.h"

#include "autostart_override.h"
#include "pipetune/dsp_pipeline.h"
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

constexpr auto kSetupSampleRate = float{48000.0F};
constexpr auto kSetupChannels = std::uint32_t{2};
constexpr auto kSetupMaximumFrames = std::uint32_t{8192};
constexpr auto kMaximumSnapshotBytes = std::size_t{64 * 1024};

struct FileSnapshot {
  bool exists;
  std::string contents;
  mode_t mode;
  std::string error;
};

static std::string processFailure(std::string_view operation,
                                  const ProcessResult &result) {
  if (!result.error.empty()) {
    return std::string(operation) + ": " + result.error;
  }
  return std::string(operation) + " exited with status " +
         std::to_string(result.exitCode);
}

static bool processSucceeded(const ProcessResult &result) {
  return result.started && result.error.empty() && result.exitCode == 0;
}

static ProcessResult invokeProcess(
    ProcessRunner runner, void *userData,
    const std::filesystem::path &executable,
    std::initializer_list<std::string_view> argumentViews,
    ProcessWaitMode mode) {
  auto arguments = std::vector<std::string>{};
  arguments.reserve(argumentViews.size());
  for (const auto argument : argumentViews) {
    arguments.emplace_back(argument);
  }
  return runner(executable, arguments, mode, userData);
}

static FileSnapshot snapshotFile(const std::filesystem::path &path) {
  struct stat status {};
  if (lstat(path.c_str(), &status) != 0) {
    if (errno == ENOENT) {
      return {.exists = false, .contents = {}, .mode = 0600, .error = {}};
    }
    return {.exists = false,
            .contents = {},
            .mode = 0600,
            .error = "cannot inspect existing startup configuration: " +
                     std::string(std::strerror(errno))};
  }
  if (!S_ISREG(status.st_mode)) {
    return {.exists = false,
            .contents = {},
            .mode = 0600,
            .error = "existing startup configuration is not a regular file"};
  }

  auto stream = std::ifstream(path, std::ios::binary);
  if (!stream) {
    return {.exists = false,
            .contents = {},
            .mode = 0600,
            .error = "cannot read existing startup configuration"};
  }
  auto contents = std::string{};
  auto buffer = std::array<char, 4096>{};
  while (stream) {
    stream.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
    const auto count = stream.gcount();
    if (count > 0) {
      contents.append(buffer.data(), static_cast<std::size_t>(count));
      if (contents.size() > kMaximumSnapshotBytes) {
        return {.exists = false,
                .contents = {},
                .mode = 0600,
                .error = "existing startup configuration exceeds 64 KiB"};
      }
    }
  }
  if (stream.bad()) {
    return {.exists = false,
            .contents = {},
            .mode = 0600,
            .error = "cannot read existing startup configuration"};
  }
  return {.exists = true,
          .contents = std::move(contents),
          .mode = status.st_mode & 0777,
          .error = {}};
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
    return std::strerror(errno);
  }
  return {};
}

static std::string restoreFileSnapshot(
    const std::filesystem::path &path, const FileSnapshot &snapshot) {
  if (!snapshot.exists) {
    auto filesystemError = std::error_code{};
    std::filesystem::remove(path, filesystemError);
    return filesystemError
               ? "cannot remove rolled-back startup configuration: " +
                     filesystemError.message()
               : std::string{};
  }

  const auto directory = path.parent_path();
  auto filesystemError = std::error_code{};
  std::filesystem::create_directories(directory, filesystemError);
  if (filesystemError) {
    return "cannot recreate startup configuration directory: " +
           filesystemError.message();
  }
  auto templatePath = (directory / ".environment.rollback.XXXXXX").string();
  auto templateBuffer =
      std::vector<char>(templatePath.begin(), templatePath.end());
  templateBuffer.push_back('\0');
  const auto descriptor = mkstemp(templateBuffer.data());
  if (descriptor < 0) {
    return "cannot create rollback configuration: " +
           std::string(std::strerror(errno));
  }
  const auto temporaryPath = std::filesystem::path(templateBuffer.data());
  auto error = std::string{};
  if (fchmod(descriptor, snapshot.mode) != 0) {
    error = "cannot restore startup configuration mode: " +
            std::string(std::strerror(errno));
  }
  if (error.empty()) {
    const auto writeError = writeAll(descriptor, snapshot.contents);
    if (!writeError.empty()) {
      error = "cannot write rollback configuration: " + writeError;
    }
  }
  if (error.empty() && fsync(descriptor) != 0) {
    error = "cannot synchronize rollback configuration: " +
            std::string(std::strerror(errno));
  }
  if (close(descriptor) != 0 && error.empty()) {
    error = "cannot close rollback configuration: " +
            std::string(std::strerror(errno));
  }
  if (error.empty() &&
      rename(temporaryPath.c_str(), path.c_str()) != 0) {
    error = "cannot replace rollback configuration: " +
            std::string(std::strerror(errno));
  }
  if (!error.empty()) {
    unlink(temporaryPath.c_str());
  }
  return error;
}

static void appendProcessRollbackWarning(
    const UserSetupRequest &request,
    std::initializer_list<std::string_view> arguments,
    std::string_view operation, std::vector<std::string> &warnings) {
  const auto result = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable, arguments, ProcessWaitMode::wait);
  if (!processSucceeded(result)) {
    warnings.push_back(processFailure(operation, result));
  }
}

static void rollbackSetup(const UserSetupRequest &request,
                          bool restoreConfiguration,
                          const FileSnapshot &configurationSnapshot,
                          bool serviceMutationStarted, bool wasEnabled,
                          bool wasActive,
                          std::vector<std::string> &warnings) {
  if (restoreConfiguration) {
    const auto error = restoreFileSnapshot(request.paths.configPath,
                                           configurationSnapshot);
    if (!error.empty()) {
      warnings.push_back("setup rollback: " + error);
    }
  }
  if (!serviceMutationStarted) {
    return;
  }
  if (wasEnabled) {
    appendProcessRollbackWarning(
        request, {"--user", "enable", "pipetune.service"},
        "setup rollback could not re-enable pipetune.service", warnings);
  } else {
    appendProcessRollbackWarning(
        request, {"--user", "disable", "pipetune.service"},
        "setup rollback could not disable pipetune.service", warnings);
  }
  if (wasActive) {
    appendProcessRollbackWarning(
        request, {"--user", "restart", "pipetune.service"},
        "setup rollback could not restart pipetune.service", warnings);
  } else {
    appendProcessRollbackWarning(
        request, {"--user", "stop", "pipetune.service"},
        "setup rollback could not stop pipetune.service", warnings);
  }
}

UserManagementPathResult resolveUserManagementPaths(
    std::string_view xdgConfigHome,
    const std::filesystem::path &homeDirectory,
    const std::filesystem::path &systemctlExecutable,
    const std::filesystem::path &gtkExecutable) {
  const auto config =
      resolveStartupConfigPath(xdgConfigHome, homeDirectory);
  if (!config.error.empty()) {
    return {.paths = {}, .error = config.error};
  }
  if (systemctlExecutable.empty() || gtkExecutable.empty()) {
    return {.paths = {},
            .error = "installed management executable paths are unavailable"};
  }
  const auto xdgRoot = config.path.parent_path().parent_path();
  const auto autostart =
      xdgRoot / "autostart" / "net.kekyo.pipetune-gtk.desktop";
  return {
      .paths =
          {
              .configPath = config.path,
              .legacyConfigPath =
                  config.path.parent_path() / "environment.gtk",
              .autostartPath = autostart,
              .autostartBackupPath =
                  autostart.string() + ".pipetune-backup",
              .systemctlExecutable = systemctlExecutable,
              .gtkExecutable = gtkExecutable,
          },
      .error = {},
  };
}

UserManagementResult executeUserSetup(const UserSetupRequest &request) {
  if (request.effectiveUserId == 0) {
    return {.success = false,
            .warnings = {},
            .error = "setup must be run as a non-root user"};
  }
  if (request.processRunner == nullptr) {
    return {.success = false,
            .warnings = {},
            .error = "setup process runner is unavailable"};
  }

  auto warnings = std::vector<std::string>{};
  if (request.presetSpecified) {
    if (!request.presetPath.is_absolute()) {
      return {.success = false,
              .warnings = {},
              .error = "setup preset path must be absolute"};
    }
    const auto loaded = loadDspPipeline(
        request.presetPath,
        {.sampleRate = kSetupSampleRate,
         .maxChannels = kSetupChannels,
         .maxFrames = kSetupMaximumFrames});
    if (loaded.pipeline == nullptr) {
      return {.success = false,
              .warnings = {},
              .error = "cannot use setup preset: " + loaded.error};
    }
    for (const auto &warning : loaded.warnings) {
      warnings.push_back(
          "preset node " + std::to_string(warning.nodeIndex) + " (\"" +
          warning.pluginName + "\") was skipped: " + warning.reason);
    }
  }

  auto configurationSnapshot =
      FileSnapshot{.exists = false, .contents = {}, .mode = 0600, .error = {}};
  if (request.presetSpecified) {
    configurationSnapshot = snapshotFile(request.paths.configPath);
    if (!configurationSnapshot.error.empty()) {
      return {.success = false,
              .warnings = std::move(warnings),
              .error = configurationSnapshot.error};
    }
  }

  const auto enabledProbe = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable,
      {"--user", "is-enabled", "--quiet", "pipetune.service"},
      ProcessWaitMode::wait);
  if (!enabledProbe.started || !enabledProbe.error.empty()) {
    return {.success = false,
            .warnings = std::move(warnings),
            .error = processFailure("cannot inspect service enablement",
                                    enabledProbe)};
  }
  const auto activeProbe = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable,
      {"--user", "is-active", "--quiet", "pipetune.service"},
      ProcessWaitMode::wait);
  if (!activeProbe.started || !activeProbe.error.empty()) {
    return {.success = false,
            .warnings = std::move(warnings),
            .error =
                processFailure("cannot inspect service activity", activeProbe)};
  }
  const auto wasEnabled = enabledProbe.exitCode == 0;
  const auto wasActive = activeProbe.exitCode == 0;

  if (request.presetSpecified) {
    const auto saveError =
        saveStartupPreset(request.paths.configPath, request.presetPath);
    if (!saveError.empty()) {
      return {.success = false,
              .warnings = std::move(warnings),
              .error = saveError};
    }
  }

  auto serviceMutationStarted = false;
  const auto failSetup = [&](std::string error) {
    rollbackSetup(request, request.presetSpecified, configurationSnapshot,
                  serviceMutationStarted, wasEnabled, wasActive, warnings);
    return UserManagementResult{.success = false,
                                .warnings = std::move(warnings),
                                .error = std::move(error)};
  };

  const auto reload = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable, {"--user", "daemon-reload"},
      ProcessWaitMode::wait);
  if (!processSucceeded(reload)) {
    return failSetup(processFailure("cannot reload systemd user units", reload));
  }

  serviceMutationStarted = true;
  const auto enable = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable,
      {"--user", "enable", "pipetune.service"}, ProcessWaitMode::wait);
  if (!processSucceeded(enable)) {
    return failSetup(processFailure("cannot enable pipetune.service", enable));
  }
  const auto restart = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable,
      {"--user", "restart", "pipetune.service"}, ProcessWaitMode::wait);
  if (!processSucceeded(restart)) {
    return failSetup(processFailure("cannot restart pipetune.service", restart));
  }
  const auto active = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable,
      {"--user", "is-active", "--quiet", "pipetune.service"},
      ProcessWaitMode::wait);
  if (!processSucceeded(active)) {
    return failSetup(
        processFailure("pipetune.service did not become active", active));
  }

  auto autostart = restoreGtkAutostart(
      request.paths.autostartPath, request.paths.autostartBackupPath);
  for (auto &warning : autostart.warnings) {
    warnings.push_back(std::move(warning));
  }
  if (!autostart.success) {
    return failSetup(autostart.error);
  }

  const auto gtk = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.gtkExecutable, {"--hidden"},
      ProcessWaitMode::detached);
  if (!processSucceeded(gtk)) {
    return failSetup(processFailure("cannot launch pipetune-gtk", gtk));
  }
  return {.success = true,
          .warnings = std::move(warnings),
          .error = {}};
}

static std::string removeConfigFile(const std::filesystem::path &path) {
  auto filesystemError = std::error_code{};
  std::filesystem::remove(path, filesystemError);
  return filesystemError ? "cannot remove " + path.string() + ": " +
                               filesystemError.message()
                         : std::string{};
}

UserManagementResult executeUserUnsetup(
    const UserUnsetupRequest &request) {
  if (request.effectiveUserId == 0) {
    return {.success = false,
            .warnings = {},
            .error = "unsetup must be run as a non-root user"};
  }
  if (request.processRunner == nullptr ||
      request.restoreDefaultSink == nullptr) {
    return {.success = false,
            .warnings = {},
            .error = "unsetup external operations are unavailable"};
  }

  auto autostart = maskGtkAutostart(
      request.paths.autostartPath, request.paths.autostartBackupPath);
  if (!autostart.success) {
    return {.success = false,
            .warnings = std::move(autostart.warnings),
            .error = autostart.error};
  }
  auto warnings = std::move(autostart.warnings);

  const auto gtk = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.gtkExecutable, {"--quit"}, ProcessWaitMode::wait);
  if (!processSucceeded(gtk)) {
    warnings.push_back(processFailure("could not stop pipetune-gtk", gtk));
  }

  const auto disable = invokeProcess(
      request.processRunner, request.processUserData,
      request.paths.systemctlExecutable,
      {"--user", "disable", "--now", "pipetune.service"},
      ProcessWaitMode::wait);
  if (!processSucceeded(disable)) {
    return {.success = false,
            .warnings = std::move(warnings),
            .error =
                processFailure("cannot disable pipetune.service", disable)};
  }

  const auto restored =
      request.restoreDefaultSink("pipetune_sink", request.restoreUserData);
  if (!restored.success) {
    return {.success = false,
            .warnings = std::move(warnings),
            .error = restored.error};
  }

  if (request.purge) {
    const auto configError = removeConfigFile(request.paths.configPath);
    if (!configError.empty()) {
      return {.success = false,
              .warnings = std::move(warnings),
              .error = configError};
    }
    const auto legacyError =
        removeConfigFile(request.paths.legacyConfigPath);
    if (!legacyError.empty()) {
      return {.success = false,
              .warnings = std::move(warnings),
              .error = legacyError};
    }
  }
  return {.success = true,
          .warnings = std::move(warnings),
          .error = {}};
}

} // namespace pipetune
