/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "command_line.h"

#include "bypass_command.h"
#include "config_reset_command.h"
#include "dsp_backend_command.h"
#include "installed_tools.h"
#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"
#include "pipetune/startup_config.h"
#include "pipetune/version.h"
#include "process_runner.h"
#include "rate_command.h"
#include "startup_pipeline.h"
#include "user_setup.h"

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

constexpr auto kMaximumProcessFrames = std::uint32_t{8192};
constexpr auto kRingCapacityFrames = std::uint32_t{16384};
constexpr auto kInitialSampleRate = std::uint32_t{48000};

static std::filesystem::path absolutePresetPath(
    const std::filesystem::path &path, std::string &error) {
  auto filesystemError = std::error_code{};
  auto absolute = std::filesystem::absolute(path, filesystemError);
  if (filesystemError) {
    error = "cannot resolve preset path: " + filesystemError.message();
    return {};
  }
  return absolute.lexically_normal();
}

static pipetune::StartupConfigPathResult resolveUserStartupConfigPath() {
  const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
  const auto *home = std::getenv("HOME");
  return pipetune::resolveStartupConfigPath(
      xdgConfigHome == nullptr ? std::string_view{}
                               : std::string_view(xdgConfigHome),
      home == nullptr ? std::filesystem::path{}
                      : std::filesystem::path(home));
}

static pipetune::ProcessResult runUserManagementProcess(
    const std::filesystem::path &executable,
    std::span<const std::string> arguments,
    pipetune::ProcessWaitMode mode, void *) {
  return pipetune::runProcess(executable, arguments, mode);
}

static int runConfigResetCommand(
    const pipetune::CommandLineOptions &options) {
  if (!options.assumeYes) {
    std::cout
        << "Reset PipeTune configuration to Bypass, Automatic, and "
           "scalar DSP? [y/N] "
        << std::flush;
    auto response = std::string{};
    if (!std::getline(std::cin, response) ||
        !pipetune::configurationResetIsConfirmed(response)) {
      std::cout << "Configuration reset cancelled.\n";
      return 0;
    }
  }

  const auto config = resolveUserStartupConfigPath();
  if (!config.error.empty()) {
    std::cerr << "pipetune: " << config.error << '\n';
    return 1;
  }
  const auto result = pipetune::executeConfigurationReset(
      {.configPath = config.path,
       .systemctlExecutable = std::filesystem::path{
           std::string(pipetune::installedSystemctlPath)},
       .processRunner = runUserManagementProcess,
       .processUserData = nullptr});
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  std::cout << "PipeTune configuration was reset to Bypass, Automatic, "
               "and scalar DSP.\n";
  return 0;
}

static pipetune::UserManagementPathResult
resolveInstalledUserManagementPaths() {
  const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
  const auto *xdgDataHome = std::getenv("XDG_DATA_HOME");
  const auto *xdgStateHome = std::getenv("XDG_STATE_HOME");
  const auto *home = std::getenv("HOME");
  return pipetune::resolveUserManagementPaths(
      xdgConfigHome == nullptr ? std::string_view{}
                               : std::string_view(xdgConfigHome),
      xdgDataHome == nullptr ? std::string_view{}
                             : std::string_view(xdgDataHome),
      xdgStateHome == nullptr ? std::string_view{}
                              : std::string_view(xdgStateHome),
      home == nullptr ? std::filesystem::path{}
                      : std::filesystem::path(home),
      std::filesystem::path{
          std::string(pipetune::installedSystemctlPath)},
      std::filesystem::path{std::string(pipetune::installedGtkPath)});
}

static void printUserManagementWarnings(
    const std::vector<std::string> &warnings) {
  for (const auto &warning : warnings) {
    std::cerr << "pipetune: warning: " << warning << '\n';
  }
}

static int runSetupCommand(
    const pipetune::CommandLineOptions &options) {
  if (geteuid() == 0) {
    std::cerr << "pipetune: setup must be run as a non-root user\n";
    return 1;
  }
  const auto paths = resolveInstalledUserManagementPaths();
  if (!paths.error.empty()) {
    std::cerr << "pipetune: " << paths.error << '\n';
    return 1;
  }

  const auto presetSpecified = !options.presetPath.empty();
  auto presetPath = std::filesystem::path{};
  if (presetSpecified) {
    auto pathError = std::string{};
    presetPath = absolutePresetPath(options.presetPath, pathError);
    if (!pathError.empty()) {
      std::cerr << "pipetune: " << pathError << '\n';
      return 1;
    }
  }
  const auto result = pipetune::executeUserSetup(
      {.effectiveUserId = static_cast<std::uint32_t>(geteuid()),
       .force = options.forceSetup,
       .launchGtk = options.launchGtk,
       .presetSpecified = presetSpecified,
       .presetPath = std::move(presetPath),
       .paths = paths.paths,
       .processRunner = runUserManagementProcess,
       .processUserData = nullptr});
  printUserManagementWarnings(result.warnings);
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  std::cout << "PipeTune is enabled and running for the current user.\n";
  return 0;
}

static int runUnsetupCommand(
    const pipetune::CommandLineOptions &options) {
  if (geteuid() == 0) {
    std::cerr << "pipetune: unsetup must be run as a non-root user\n";
    return 1;
  }
  const auto paths = resolveInstalledUserManagementPaths();
  if (!paths.error.empty()) {
    std::cerr << "pipetune: " << paths.error << '\n';
    return 1;
  }
  const auto result = pipetune::executeUserUnsetup(
      {.effectiveUserId = static_cast<std::uint32_t>(geteuid()),
       .purge = options.purge,
       .paths = paths.paths,
       .processRunner = runUserManagementProcess,
       .processUserData = nullptr});
  printUserManagementWarnings(result.warnings);
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  std::cout << "PipeTune is disabled for the current user";
  if (options.purge) {
    std::cout << " and its application configuration was removed";
  }
  std::cout << ".\n";
  return 0;
}

static int runControlClient(const pipetune::CommandLineOptions &options) {
  const auto socket =
      pipetune::resolveControlSocketPath(options.controlSocketPath);
  if (!socket.error.empty()) {
    std::cerr << "pipetune: " << socket.error << '\n';
    return 1;
  }

  auto request = std::string{};
  if (options.action == pipetune::CommandLineAction::status) {
    request = pipetune::makeStatusControlRequest();
  } else {
    auto pathError = std::string{};
    const auto preset = absolutePresetPath(options.presetPath, pathError);
    if (!pathError.empty()) {
      std::cerr << "pipetune: " << pathError << '\n';
      return 1;
    }
    request = pipetune::makeLoadPresetControlRequest(preset);
    if (request.empty()) {
      std::cerr << "pipetune: cannot encode live preset request\n";
      return 1;
    }
  }

  const auto exchange =
      pipetune::exchangeControlMessage(socket.path, request);
  if (!exchange.error.empty()) {
    std::cerr << "pipetune: " << exchange.error << '\n';
    return 1;
  }
  const auto inspection =
      pipetune::inspectControlResponse(exchange.response);
  if (!inspection.valid) {
    std::cerr << "pipetune: " << inspection.error << '\n';
    return 1;
  }
  std::cout << exchange.response << '\n';
  return inspection.success ? 0 : 1;
}

static int runDaemon(const pipetune::CommandLineOptions &options) {
  auto configPath = options.configPath;
  if (configPath.empty()) {
    const auto resolved = resolveUserStartupConfigPath();
    if (!resolved.error.empty()) {
      std::cerr << "pipetune: " << resolved.error << '\n';
      return 1;
    }
    configPath = resolved.path;
  }

  auto prepared = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = static_cast<float>(kInitialSampleRate),
       .maxChannels = 2,
       .maxFrames = kMaximumProcessFrames});
  if (prepared.pipeline == nullptr) {
    std::cerr << "pipetune: " << prepared.error << '\n';
    return 1;
  }
  if (!prepared.configurationError.empty()) {
    std::cerr << "pipetune: warning: " << prepared.configurationError
              << "; DSP processing is bypassed\n";
  }
  if (!prepared.dspBackendError.empty()) {
    std::cerr << "pipetune: warning: " << prepared.dspBackendError;
    if (prepared.effectiveDspVariant.has_value()) {
      std::cerr << "; using DSP variant "
                << pipetune::dspBackendVariantName(
                       *prepared.effectiveDspVariant);
    } else {
      std::cerr << "; DSP presets are unavailable";
    }
    std::cerr << '\n';
  }
  for (const auto &warning : prepared.warnings) {
    std::cerr << "pipetune: warning: preset node " << warning.nodeIndex
              << " (\"" << warning.pluginName
              << "\") was skipped: " << warning.reason << '\n';
  }
  const auto initialDspSampleRate = static_cast<std::uint32_t>(
      prepared.pipeline->sampleRate());

  const auto socket = pipetune::resolveControlSocketPath({});
  if (!socket.error.empty()) {
    std::cerr << "pipetune: " << socket.error << '\n';
    return 1;
  }
  const auto result = pipetune::runPipeWirePipeline(
      std::move(prepared.pipeline),
      {.filterName = "pipetune_sink",
       .filterDescription = "PipeTune Processed Audio",
       .initialPresetPath = prepared.activePresetPath,
       .initialConfigurationError = prepared.configurationError,
       .controlSocketPath = socket.path,
       .dspSampleRate = initialDspSampleRate,
       .ratePolicy = prepared.ratePolicy,
       .channelCount = 2,
       .maxFrames = kMaximumProcessFrames,
       .ringCapacityFrames = kRingCapacityFrames,
       .readyCallback = nullptr,
       .readyUserData = nullptr,
       .dspBackends = std::move(prepared.dspBackends),
       .configuredDspBackend = prepared.configuredDspBackend,
       .configuredDspSimdVariant =
           prepared.configuredDspSimdVariant},
      pipetune::PipeWireRunMode::untilInterrupted);
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  if (result.overrunFrames != 0 || result.underrunFrames != 0 ||
      result.processingErrors != 0) {
    std::cerr << "pipetune: audio bridge summary: " << result.overrunFrames
              << " overrun frames, " << result.underrunFrames
              << " underrun frames, " << result.processingErrors
              << " DSP processing errors\n";
  }
  return 0;
}

static int runPersistentBypass(
    const pipetune::CommandLineOptions &options) {
  const auto config = resolveUserStartupConfigPath();
  if (!config.error.empty()) {
    std::cerr << "pipetune: " << config.error << '\n';
    return 1;
  }
  const auto socket =
      pipetune::resolveControlSocketPath(options.controlSocketPath);
  if (!socket.error.empty()) {
    std::cerr << "pipetune: " << socket.error << '\n';
    return 1;
  }

  const auto result = pipetune::executePersistentBypass(
      {.configPath = config.path, .socketPath = socket.path});
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  if (result.liveApplied) {
    std::cout << "DSP bypass is active and saved for future starts.\n";
  } else {
    std::cout << "DSP bypass is saved for the next daemon start";
    if (!result.notice.empty()) {
      std::cout << " (" << result.notice << ')';
    }
    std::cout << ".\n";
  }
  return 0;
}

static bool isRateCommand(pipetune::CommandLineAction action) {
  return action == pipetune::CommandLineAction::rateGet ||
         action == pipetune::CommandLineAction::rateList ||
         action == pipetune::CommandLineAction::rateSet;
}

static bool isDspCommand(pipetune::CommandLineAction action) {
  return action == pipetune::CommandLineAction::dspList ||
         action == pipetune::CommandLineAction::dspGet ||
         action == pipetune::CommandLineAction::dspSet;
}

static int runDspCommand(const pipetune::CommandLineOptions &options) {
  const auto socket =
      pipetune::resolveControlSocketPath(options.controlSocketPath);
  if (!socket.error.empty()) {
    std::cerr << "pipetune: " << socket.error << '\n';
    return 1;
  }

  if (options.action == pipetune::CommandLineAction::dspList ||
      options.action == pipetune::CommandLineAction::dspGet) {
    const auto queried = pipetune::queryDspBackendStatus(socket.path);
    if (!queried.success) {
      std::cerr << "pipetune: " << queried.error << '\n';
      return 1;
    }
    if (options.json) {
      std::cout << queried.json << '\n';
    } else if (options.action ==
               pipetune::CommandLineAction::dspList) {
      std::cout << pipetune::formatDspBackendList(queried.status);
    } else {
      std::cout << pipetune::formatDspBackendStatus(queried.status);
    }
    return 0;
  }

  const auto config = resolveUserStartupConfigPath();
  if (!config.error.empty()) {
    std::cerr << "pipetune: " << config.error << '\n';
    return 1;
  }
  const auto changed = pipetune::executeSetDspBackend(
      {.configPath = config.path, .socketPath = socket.path},
      options.dspBackend, options.dspSimdVariant);
  if (!changed.success) {
    std::cerr << "pipetune: " << changed.error << '\n';
    return 1;
  }
  if (changed.liveApplied) {
    std::cout << "DSP backend is active and saved for future starts.\n"
              << pipetune::formatDspBackendStatus(changed.status);
  } else {
    std::cout << "DSP backend is saved for the next daemon start";
    if (!changed.notice.empty()) {
      std::cout << " (" << changed.notice << ')';
    }
    std::cout << ".\n";
  }
  return 0;
}

static int runRateCommand(
    const pipetune::CommandLineOptions &options) {
  const auto socket =
      pipetune::resolveControlSocketPath(options.controlSocketPath);
  if (!socket.error.empty()) {
    std::cerr << "pipetune: " << socket.error << '\n';
    return 1;
  }

  if (options.action == pipetune::CommandLineAction::rateGet ||
      options.action == pipetune::CommandLineAction::rateList) {
    const auto queried = pipetune::queryRateStatus(socket.path);
    if (!queried.success) {
      std::cerr << "pipetune: " << queried.error << '\n';
      return 1;
    }
    if (options.json) {
      std::cout << queried.json << '\n';
    } else if (options.action ==
               pipetune::CommandLineAction::rateList) {
      std::cout << pipetune::formatSelectableSampleRates();
    } else {
      std::cout << pipetune::formatSampleRateStatus(queried.status);
    }
    return 0;
  }

  const auto config = resolveUserStartupConfigPath();
  if (!config.error.empty()) {
    std::cerr << "pipetune: " << config.error << '\n';
    return 1;
  }
  const auto change = pipetune::executeSetSampleRatePolicy(
      {.configPath = config.path, .socketPath = socket.path},
      options.ratePolicy);
  if (!change.success) {
    std::cerr << "pipetune: " << change.error << '\n';
    return 1;
  }
  if (change.liveApplied) {
    std::cout << "Sample-rate policy is active and saved for future starts.\n"
              << pipetune::formatSampleRateStatus(change.status);
  } else {
    std::cout << "Sample-rate policy is saved for the next daemon start";
    if (!change.notice.empty()) {
      std::cout << " (" << change.notice << ')';
    }
    std::cout << ".\n";
  }
  return 0;
}

int main(int argc, char **argv) {
  auto arguments = std::vector<std::string_view>{};
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (auto index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  const auto parsed = pipetune::parseCommandLine(arguments);
  if (!parsed.error.empty()) {
    std::cerr << "pipetune: " << parsed.error << "\n\n"
              << pipetune::commandLineUsage();
    return 2;
  }
  if (parsed.options.action == pipetune::CommandLineAction::help) {
    std::cout << pipetune::commandLineUsage();
    return 0;
  }
  if (parsed.options.action == pipetune::CommandLineAction::version) {
    std::cout << "PipeTune " << pipetune::version() << ", EffeTune DSP "
              << pipetune::effetuneVersion() << '\n';
    return 0;
  }
  if (parsed.options.action == pipetune::CommandLineAction::daemon) {
    return runDaemon(parsed.options);
  }
  if (parsed.options.action == pipetune::CommandLineAction::bypass) {
    return runPersistentBypass(parsed.options);
  }
  if (isRateCommand(parsed.options.action)) {
    return runRateCommand(parsed.options);
  }
  if (isDspCommand(parsed.options.action)) {
    return runDspCommand(parsed.options);
  }
  if (parsed.options.action ==
      pipetune::CommandLineAction::configReset) {
    return runConfigResetCommand(parsed.options);
  }
  if (parsed.options.action == pipetune::CommandLineAction::setup) {
    return runSetupCommand(parsed.options);
  }
  if (parsed.options.action == pipetune::CommandLineAction::unsetup) {
    return runUnsetupCommand(parsed.options);
  }
  if (parsed.options.action == pipetune::CommandLineAction::loadPreset ||
      parsed.options.action == pipetune::CommandLineAction::status) {
    return runControlClient(parsed.options);
  }

  auto pathError = std::string{};
  const auto presetPath =
      absolutePresetPath(parsed.options.presetPath, pathError);
  if (!pathError.empty()) {
    std::cerr << "pipetune: " << pathError << '\n';
    return 1;
  }

  auto backends = pipetune::discoverDspBackends();
  const auto selected = pipetune::selectDspBackend(
      parsed.options.dspBackend, parsed.options.dspSimdVariant,
      backends);
  if (selected.effectiveBackend == nullptr) {
    std::cerr << "pipetune: " << selected.error << '\n';
    return 1;
  }
  if (selected.fallback) {
    std::cerr << "pipetune: warning: " << selected.error
              << "; using scalar DSP backend\n";
  }
  const auto initialDspSampleRate = pipetune::dspSampleRateForPolicy(
      parsed.options.ratePolicy, kInitialSampleRate);
  auto loaded = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = static_cast<float>(initialDspSampleRate),
       .maxChannels = parsed.options.channelCount,
       .maxFrames = kMaximumProcessFrames},
      selected.effectiveBackend);
  if (loaded.pipeline == nullptr) {
    std::cerr << "pipetune: " << loaded.error << '\n';
    return 1;
  }
  for (const auto &warning : loaded.warnings) {
    std::cerr << "pipetune: warning: preset node " << warning.nodeIndex << " (\""
              << warning.pluginName << "\") was skipped: " << warning.reason
              << '\n';
  }

  const auto mode = parsed.options.checkOnly
                        ? pipetune::PipeWireRunMode::untilReady
                        : pipetune::PipeWireRunMode::untilInterrupted;
  auto controlSocket = std::filesystem::path{};
  if (!parsed.options.checkOnly) {
    const auto resolved =
        pipetune::resolveControlSocketPath(parsed.options.controlSocketPath);
    if (!resolved.error.empty()) {
      std::cerr << "pipetune: " << resolved.error << '\n';
      return 1;
    }
    controlSocket = resolved.path;
  }
  const auto result = pipetune::runPipeWirePipeline(
      std::move(loaded.pipeline),
      {.filterName = "pipetune_sink",
       .filterDescription = "PipeTune Processed Audio",
       .initialPresetPath = presetPath,
       .initialConfigurationError = {},
       .controlSocketPath = controlSocket,
       .dspSampleRate = initialDspSampleRate,
       .ratePolicy = parsed.options.ratePolicy,
       .channelCount = parsed.options.channelCount,
       .maxFrames = kMaximumProcessFrames,
       .ringCapacityFrames = kRingCapacityFrames,
       .readyCallback = nullptr,
       .readyUserData = nullptr,
       .dspBackends = std::move(backends),
       .configuredDspBackend = parsed.options.dspBackend,
       .configuredDspSimdVariant =
           parsed.options.dspSimdVariant},
      mode);
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  if (parsed.options.checkOnly) {
    std::cout << "PipeWire filter is ready: pipetune_sink\n";
  }
  if (result.overrunFrames != 0 || result.underrunFrames != 0 ||
      result.processingErrors != 0) {
    std::cerr << "pipetune: audio bridge summary: " << result.overrunFrames
              << " overrun frames, " << result.underrunFrames
              << " underrun frames, " << result.processingErrors
              << " DSP processing errors\n";
  }
  return 0;
}
