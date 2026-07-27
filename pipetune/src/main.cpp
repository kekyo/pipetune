#include "command_line.h"

#include "default_sink_restore.h"
#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"
#include "pipetune/version.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

constexpr auto kMaximumProcessFrames = std::uint32_t{8192};
constexpr auto kRingCapacityFrames = std::uint32_t{16384};

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
    std::cout << "pipetune " << pipetune::version() << '\n';
    return 0;
  }
  if (parsed.options.action ==
      pipetune::CommandLineAction::restoreDefault) {
    const auto restored =
        pipetune::restorePipeWireDefaultSink(parsed.options.sinkName);
    if (!restored.success) {
      std::cerr << "pipetune: " << restored.error << '\n';
      return 1;
    }
    std::cout << "Restored PipeWire default sink: "
              << restored.selectedTarget << '\n';
    return 0;
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

  auto loaded = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = static_cast<float>(parsed.options.sampleRate),
       .maxChannels = parsed.options.channelCount,
       .maxFrames = kMaximumProcessFrames});
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
      {.sinkName = parsed.options.sinkName,
       .sinkDescription = "PipeTune Processed Audio",
       .targetObject = parsed.options.targetObject,
       .initialPresetPath = presetPath,
       .initialConfigurationError = {},
       .controlSocketPath = controlSocket,
       .sampleRate = parsed.options.sampleRate,
       .channelCount = parsed.options.channelCount,
       .maxFrames = kMaximumProcessFrames,
       .ringCapacityFrames = kRingCapacityFrames,
       .manageDefaultSink = !parsed.options.checkOnly,
       .readyCallback = nullptr,
       .readyUserData = nullptr},
      mode);
  if (!result.success) {
    std::cerr << "pipetune: " << result.error << '\n';
    return 1;
  }
  if (parsed.options.checkOnly) {
    std::cout << "PipeWire pipeline is ready: " << parsed.options.sinkName << '\n';
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
