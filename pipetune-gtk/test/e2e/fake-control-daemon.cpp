#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <cstdint>
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
  std::string configurationError;
  std::filesystem::path requestLogPath;
  std::string rejectedCommand;
  std::uint64_t dspTelemetrySequence;
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

static pipetune::ControlFilterOutputStatus makeFilterOutput(
    std::string nodeName, std::string description, std::string filterName,
    std::uint32_t channelCount, std::uint32_t sampleRate) {
  return {
      .targetNodeName = std::move(nodeName),
      .targetDescription = std::move(description),
      .filterNodeName = std::move(filterName),
      .state = pipetune::ControlFilterState::active,
      .error = {},
      .channelCount = channelCount,
      .sampleRateCapabilities = sampleRateCapabilities(),
      .dspSampleRate = sampleRate,
      .outputSampleRate = sampleRate,
      .activeOutputSampleRate = sampleRate,
      .rateFallback = false,
      .latencyFrames = 128,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 128000,
      .dspProcessingNanoseconds = 200000000,
  };
}

static pipetune::ControlRuntimeStatus
makeStatus(const pipetune::StartupConfig &config,
           std::string_view configurationError) {
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
      .configurationError = std::string(configurationError),
      .activePluginCount = config.presetFound ? 5u : 0u,
      .policyBackend = "wireplumber-0.5",
      .filterOutputs =
          {makeFilterOutput(
               "alsa_output.usb-long-studio-dac.analog-stereo",
               "Studio DAC", "pipetune.filter.usb-studio-dac", 2,
               dspRate),
           makeFilterOutput(
               "alsa_output.pci-0000_0b_00.4.hdmi-stereo-extra-long",
               "Studio Display", "pipetune.filter.hdmi-display", 8,
               dspRate),
           makeFilterOutput(
               "alsa_output.bluetooth-living-room-speaker.a2dp-sink",
               "Living Room Speaker", "pipetune.filter.bluetooth", 2,
               dspRate)},
      .overrunFrames = 2,
      .underrunFrames = 3,
      .processingErrors = 1,
      .dspProcessedFrames = 384000,
      .dspProcessingNanoseconds = 600000000,
      .configuredRatePolicy = config.ratePolicy,
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
  };
}

static pipetune::ControlRuntimeStatus snapshotStatus(
    FakeDaemonState &state) {
  auto lock = std::scoped_lock(state.mutex);
  auto status = makeStatus(state.liveConfig, state.configurationError);
  const auto framesPerInterval =
      status.filterOutputs.empty()
          ? std::uint64_t{0}
          : static_cast<std::uint64_t>(
                status.filterOutputs.front().dspSampleRate) *
                status.filterOutputs.size();
  status.dspProcessedFrames +=
      state.dspTelemetrySequence * framesPerInterval;
  status.dspProcessingNanoseconds +=
      state.dspTelemetrySequence * std::uint64_t{600000000};
  ++state.dspTelemetrySequence;
  return status;
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
  case pipetune::ControlCommand::setRate:
    return "setRate";
  case pipetune::ControlCommand::setDspBackend:
    return "setDspBackend";
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
    case pipetune::ControlCommand::setRate:
      state.liveConfig.ratePolicy = parsed.request.ratePolicy;
      break;
    case pipetune::ControlCommand::setDspBackend:
      state.liveConfig.dspBackend = parsed.request.dspBackend;
      state.liveConfig.dspSimdVariant = parsed.request.dspSimdVariant;
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

  auto state = FakeDaemonState{
      .mutex = {},
      .liveConfig = loaded.config,
      .configurationError = loaded.error,
      .requestLogPath = environmentValue("PIPETUNE_E2E_REQUEST_LOG"),
      .rejectedCommand = environmentValue("PIPETUNE_E2E_REJECT_COMMAND"),
      .dspTelemetrySequence = 0,
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
