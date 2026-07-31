#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct FakeDaemonState {
  std::mutex mutex;
  pipetune::StartupConfig liveConfig;
  std::filesystem::path requestLogPath;
  std::string rejectedCommand;
};

static std::string environmentValue(const char *name) {
  const auto *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
}

static pipetune::SampleRateCapabilities sampleRateCapabilities() {
  return {
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::range,
            .minimum = 44100,
            .maximum = 384000,
            .step = 0}},
  };
}

static std::optional<pipetune::DspBackendVariant>
effectiveVariant(const pipetune::StartupConfig &config) {
  if (config.dspBackend == pipetune::DspBackendKind::scalar) {
    return pipetune::DspBackendVariant::scalar;
  }
  const auto pinned =
      pipetune::concreteDspBackendVariant(config.dspSimdVariant);
  return pinned.has_value()
             ? pinned
             : std::optional<pipetune::DspBackendVariant>(
                   pipetune::DspBackendVariant::x86_64_v4);
}

static pipetune::ControlRuntimeStatus
makeStatus(const pipetune::StartupConfig &config) {
  constexpr std::string_view defaultOutput =
      "alsa_output.usb-long-studio-dac.analog-stereo";
  const auto outputNames = std::vector<std::pair<std::string, std::string>>{
      {std::string(defaultOutput), "Studio DAC"},
      {"alsa_output.pci-0000_0b_00.4.hdmi-stereo-extra-long",
       "Studio DAC"},
      {"alsa_output.bluetooth-living-room-speaker.a2dp-sink",
       "Living Room Speaker"},
  };
  auto selectedOutput = std::string(defaultOutput);
  auto selectionReason =
      pipetune::ControlOutputSelectionReason::systemDefault;
  if (config.preferredOutputFound) {
    const auto preferred = std::find_if(
        outputNames.begin(), outputNames.end(),
        [&config](const auto &output) {
          return output.first == config.preferredOutput;
        });
    if (preferred == outputNames.end()) {
      selectionReason = pipetune::ControlOutputSelectionReason::fallback;
    } else {
      selectedOutput = preferred->first;
      selectionReason = pipetune::ControlOutputSelectionReason::preferred;
    }
  }

  auto outputs = std::vector<pipetune::ControlOutputDevice>{};
  outputs.reserve(outputNames.size());
  for (const auto &[name, description] : outputNames) {
    outputs.push_back(
        {.name = name,
         .description = description,
         .systemDefault = name == defaultOutput,
         .preferred =
             config.preferredOutputFound &&
             name == config.preferredOutput,
         .selected = name == selectedOutput,
         .sampleRateCapabilities = sampleRateCapabilities()});
  }

  const auto dspRate =
      config.ratePolicy.mode == pipetune::SampleRateMode::maximum
          ? 384000u
          : config.ratePolicy.fixedRate;
  const auto variant = effectiveVariant(config);
  return {
      .processingMode = config.presetFound
                            ? pipetune::ProcessingMode::preset
                            : pipetune::ProcessingMode::bypass,
      .activePreset =
          config.presetFound ? config.presetPath.string() : std::string{},
      .configurationError = {},
      .activePluginCount = config.presetFound ? 5u : 0u,
      .preferredTarget = config.preferredOutputFound
                             ? config.preferredOutput
                             : std::string{},
      .selectedTarget = selectedOutput,
      .outputSelectionReason = selectionReason,
      .availableOutputs = std::move(outputs),
      .defaultSinkActive = true,
      .overrunFrames = 2,
      .underrunFrames = 3,
      .processingErrors = 1,
      .dspProcessedFrames = 384000,
      .dspProcessingNanoseconds = 600000000,
      .inputSampleFormat = "F32P",
      .inputSampleRate = dspRate,
      .inputChannelCount = 2,
      .inputFramesReceived = 768000,
      .inputLastReceivedUnixMilliseconds = 1720000000123,
      .configuredRatePolicy = config.ratePolicy,
      .dspSampleRate = dspRate,
      .selectedOutputSampleRate = dspRate,
      .activeOutputSampleRate = dspRate,
      .rateTransitioning = false,
      .rateFallback = false,
      .rateError = {},
      .configuredDspBackend = config.dspBackend,
      .configuredDspSimdVariant = config.dspSimdVariant,
      .effectiveDspBackend = config.dspBackend,
      .effectiveDspVariant = variant,
      .dspBackendFallback = false,
      .dspBackendError = {},
      .availableDspBackends =
          {{
              {.kind = pipetune::DspBackendKind::scalar,
               .available = true,
               .cpuRequirement = "none",
               .error = {}},
              {.kind = pipetune::DspBackendKind::simd,
               .available = true,
               .cpuRequirement = "E2E SIMD CPU",
               .error = {}},
          }},
      .availableDspVariants =
          {{.variant = pipetune::DspBackendVariant::scalar,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "none",
            .error = {}},
           {.variant = pipetune::DspBackendVariant::simdBaseline,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "baseline SIMD",
            .error = {}},
           {.variant = pipetune::DspBackendVariant::x86_64_v3,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "x86-64-v3",
            .error = {}},
           {.variant = pipetune::DspBackendVariant::x86_64_v4,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "x86-64-v4",
            .error = {}},
           {.variant = pipetune::DspBackendVariant::arm64Sve,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "Arm SVE",
            .error = {}}},
      .dspIdlePolicy = config.dspIdlePolicy,
      .dspIdleState = pipetune::DspIdleState::active,
      .dspIdleSkippedFrames = 192,
      .dspIdleSleepTransitions = 4,
      .pipeWireIdle = false,
  };
}

static pipetune::ControlRuntimeStatus snapshotStatus(
    FakeDaemonState &state) {
  auto lock = std::scoped_lock(state.mutex);
  return makeStatus(state.liveConfig);
}

static std::string provideStatus(void *userData) {
  auto &state = *static_cast<FakeDaemonState *>(userData);
  return pipetune::makeControlStatusEvent(snapshotStatus(state));
}

static std::string commandName(pipetune::ControlCommand command) {
  switch (command) {
  case pipetune::ControlCommand::status:
    return "status";
  case pipetune::ControlCommand::loadPreset:
    return "loadPreset";
  case pipetune::ControlCommand::bypass:
    return "bypass";
  case pipetune::ControlCommand::setOutput:
    return "setOutput";
  case pipetune::ControlCommand::clearOutput:
    return "clearOutput";
  case pipetune::ControlCommand::setRate:
    return "setRate";
  case pipetune::ControlCommand::setDspBackend:
    return "setDspBackend";
  case pipetune::ControlCommand::setDspIdlePolicy:
    return "setDspIdlePolicy";
  case pipetune::ControlCommand::subscribe:
    return "subscribe";
  }
  return "unknown";
}

static void appendRequestLog(FakeDaemonState &state,
                             std::string_view message) {
  if (state.requestLogPath.empty()) {
    return;
  }
  auto stream = std::ofstream(state.requestLogPath, std::ios::app);
  if (stream) {
    stream << message << '\n';
  }
}

static pipetune::ControlMessageResult closeResponse(std::string response,
                                                    bool publishStatus) {
  return {.response = std::move(response),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = publishStatus};
}

static pipetune::ControlMessageResult handleRequest(
    std::string_view message, void *userData) {
  auto &state = *static_cast<FakeDaemonState *>(userData);
  {
    auto lock = std::scoped_lock(state.mutex);
    appendRequestLog(state, message);
  }
  const auto parsed = pipetune::parseControlRequest(message);
  if (!parsed.error.empty()) {
    return closeResponse(pipetune::makeControlErrorResponse(parsed.error),
                         false);
  }
  const auto name = commandName(parsed.request.command);
  if (state.rejectedCommand == "all" || state.rejectedCommand == name) {
    return closeResponse(
        pipetune::makeControlErrorResponse("E2E rejected " + name), false);
  }
  if (parsed.request.command == pipetune::ControlCommand::subscribe) {
    return {.response = provideStatus(&state),
            .connectionMode = pipetune::ControlConnectionMode::subscribe,
            .publishStatus = false};
  }
  if (parsed.request.command == pipetune::ControlCommand::status) {
    return closeResponse(pipetune::makeControlSuccessResponse(
                             snapshotStatus(state),
                             std::span<const pipetune::ControlWarning>{}),
                         false);
  }

  {
    auto lock = std::scoped_lock(state.mutex);
    switch (parsed.request.command) {
    case pipetune::ControlCommand::loadPreset:
      state.liveConfig.presetFound = true;
      state.liveConfig.presetPath = parsed.request.presetPath;
      break;
    case pipetune::ControlCommand::bypass:
      state.liveConfig.presetFound = false;
      state.liveConfig.presetPath.clear();
      break;
    case pipetune::ControlCommand::setOutput:
      state.liveConfig.preferredOutputFound = true;
      state.liveConfig.preferredOutput = parsed.request.outputTarget;
      break;
    case pipetune::ControlCommand::clearOutput:
      state.liveConfig.preferredOutputFound = false;
      state.liveConfig.preferredOutput.clear();
      break;
    case pipetune::ControlCommand::setRate:
      state.liveConfig.ratePolicy = parsed.request.ratePolicy;
      break;
    case pipetune::ControlCommand::setDspBackend:
      state.liveConfig.dspBackend = parsed.request.dspBackend;
      state.liveConfig.dspSimdVariant = parsed.request.dspSimdVariant;
      break;
    case pipetune::ControlCommand::setDspIdlePolicy:
      state.liveConfig.dspIdlePolicy = parsed.request.dspIdlePolicy;
      break;
    case pipetune::ControlCommand::status:
    case pipetune::ControlCommand::subscribe:
      break;
    }
  }
  return closeResponse(
      pipetune::makeControlSuccessResponse(
          snapshotStatus(state),
          std::span<const pipetune::ControlWarning>{}),
      true);
}

static std::string jsonString(std::string_view value) {
  auto escaped = std::string{"\""};
  for (const auto character : value) {
    switch (character) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }
  escaped += '"';
  return escaped;
}

static int inspectConfig(const std::filesystem::path &path) {
  const auto loaded = pipetune::loadStartupConfig(path);
  if (!loaded.error.empty()) {
    std::cerr << loaded.error << '\n';
    return 1;
  }
  const auto &config = loaded.config;
  std::cout
      << "{\"preset\":"
      << (config.presetFound ? jsonString(config.presetPath.string())
                             : "null")
      << ",\"preferredOutput\":"
      << (config.preferredOutputFound
              ? jsonString(config.preferredOutput)
              : "null")
      << ",\"rateMode\":"
      << jsonString(pipetune::sampleRateModeName(config.ratePolicy.mode))
      << ",\"fixedRate\":" << config.ratePolicy.fixedRate
      << ",\"rateEnforcement\":"
      << jsonString(pipetune::sampleRateEnforcementName(
             config.ratePolicy.enforcement))
      << ",\"dspBackend\":"
      << jsonString(pipetune::dspBackendName(config.dspBackend))
      << ",\"dspSimdVariant\":"
      << jsonString(pipetune::dspSimdVariantName(config.dspSimdVariant))
      << ",\"dspIdlePolicy\":"
      << jsonString(pipetune::dspIdlePolicyName(config.dspIdlePolicy))
      << "}\n";
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--inspect-config") {
    return inspectConfig(argv[2]);
  }
  if (argc != 1) {
    std::cerr << "usage: pipetune-gtk-e2e-daemon "
                 "[--inspect-config PATH]\n";
    return 1;
  }

  const auto socketPath = pipetune::resolveControlSocketPath({});
  if (!socketPath.error.empty()) {
    std::cerr << socketPath.error << '\n';
    return 1;
  }
  const auto configPath = pipetune::resolveStartupConfigPath(
      environmentValue("XDG_CONFIG_HOME"), environmentValue("HOME"));
  if (!configPath.error.empty()) {
    std::cerr << configPath.error << '\n';
    return 1;
  }
  const auto loaded = pipetune::loadStartupConfig(configPath.path);
  if (!loaded.error.empty()) {
    std::cerr << loaded.error << '\n';
    return 1;
  }

  auto state = FakeDaemonState{
      .mutex = {},
      .liveConfig = loaded.config,
      .requestLogPath = environmentValue("PIPETUNE_E2E_REQUEST_LOG"),
      .rejectedCommand =
          environmentValue("PIPETUNE_E2E_REJECT_COMMAND"),
  };
  auto started = pipetune::startControlServer(
      socketPath.path,
      {.handler = handleRequest,
       .statusProvider = provideStatus,
       .userData = &state});
  if (started.server == nullptr) {
    std::cerr << started.error << '\n';
    return 1;
  }

  std::cout << "READY\n" << std::flush;
  auto input = std::string{};
  while (std::getline(std::cin, input)) {
  }
  started.server.reset();
  return 0;
}
