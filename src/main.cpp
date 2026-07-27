#include "command_line.h"

#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"
#include "pipetune/version.h"

#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

constexpr auto kMaximumProcessFrames = std::uint32_t{8192};
constexpr auto kRingCapacityFrames = std::uint32_t{16384};

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

  auto loaded = pipetune::loadDspPipeline(
      parsed.options.presetPath,
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
  const auto result = pipetune::runPipeWirePipeline(
      *loaded.pipeline,
      {.sinkName = parsed.options.sinkName,
       .sinkDescription = "PipeTune Processed Audio",
       .targetObject = parsed.options.targetObject,
       .sampleRate = parsed.options.sampleRate,
       .channelCount = parsed.options.channelCount,
       .maxFrames = kMaximumProcessFrames,
       .ringCapacityFrames = kRingCapacityFrames,
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
