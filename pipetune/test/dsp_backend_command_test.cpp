#include "dsp_backend_command.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <string_view>
#include <unistd.h>

struct ServerState {
  std::mutex mutex;
  bool rejectChanges;
  pipetune::DspBackendKind backend;
  pipetune::DspSimdVariant simdVariant;
  std::size_t setRequests;
};

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlRuntimeStatus serverStatus(ServerState &state) {
  auto lock = std::scoped_lock(state.mutex);
  return {
      .processingMode = pipetune::ProcessingMode::bypass,
      .activePreset = {},
      .configurationError = {},
      .activePluginCount = 0,
      .preferredTarget = {},
      .selectedTarget = {},
      .outputSelectionReason =
          pipetune::ControlOutputSelectionReason::unavailable,
      .availableOutputs = {},
      .defaultSinkActive = false,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
      .inputSampleFormat = {},
      .inputSampleRate = 0,
      .inputChannelCount = 0,
      .inputFramesReceived = 0,
      .inputLastReceivedUnixMilliseconds = 0,
      .configuredDspBackend = state.backend,
      .configuredDspSimdVariant = state.simdVariant,
      .effectiveDspBackend = state.backend,
      .effectiveDspVariant =
          state.backend == pipetune::DspBackendKind::scalar
              ? pipetune::DspBackendVariant::scalar
              : pipetune::concreteDspBackendVariant(
                    state.simdVariant)
                    .value_or(
                        pipetune::DspBackendVariant::simdBaseline),
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
               .cpuRequirement = "test SIMD ISA",
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
            .cpuRequirement = "test SIMD ISA",
            .error = {}},
           {.variant = pipetune::DspBackendVariant::x86_64_v3,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "x86-64-v3",
            .error = {}}},
  };
}

static std::string provideStatus(void *userData) {
  auto &state = *static_cast<ServerState *>(userData);
  return pipetune::makeControlStatusEvent(serverStatus(state));
}

static pipetune::ControlMessageResult handleRequest(
    std::string_view message, void *userData) {
  auto &state = *static_cast<ServerState *>(userData);
  const auto request = pipetune::parseControlRequest(message);
  if (!request.error.empty()) {
    return {.response = pipetune::makeControlErrorResponse(request.error),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }
  if (request.request.command == pipetune::ControlCommand::status) {
    return {.response =
                pipetune::makeControlSuccessResponse(serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }
  if (request.request.command !=
      pipetune::ControlCommand::setDspBackend) {
    return {.response =
                pipetune::makeControlErrorResponse("unexpected command"),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    if (state.rejectChanges) {
      return {.response = pipetune::makeControlErrorResponse(
                  "DSP backend change rejected"),
              .connectionMode = pipetune::ControlConnectionMode::close,
              .publishStatus = false};
    }
    state.backend = request.request.dspBackend;
    state.simdVariant = request.request.dspSimdVariant;
    ++state.setRequests;
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = true};
}

static bool configHasBackendSelection(
    const std::filesystem::path &configPath,
    pipetune::DspBackendKind expectedKind,
    pipetune::DspSimdVariant expectedVariant =
        pipetune::DspSimdVariant::automatic) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.dspBackend == expectedKind,
               "stored DSP backend differs") &&
         check(loaded.dspSimdVariant == expectedVariant,
               "stored DSP SIMD variant differs");
}

static bool testStatusAndFormatting(
    const std::filesystem::path &socketPath) {
  const auto queried = pipetune::queryDspBackendStatus(socketPath);
  if (!check(queried.success, queried.error) ||
      !check(!queried.json.empty(),
             "DSP backend query must preserve JSON")) {
    return false;
  }
  const auto status = pipetune::formatDspBackendStatus(queried.status);
  const auto list = pipetune::formatDspBackendList(queried.status);
  return check(status.find("Configured backend: scalar") !=
                   std::string::npos &&
                   status.find("Effective backend: scalar") !=
                       std::string::npos,
               "formatted DSP backend status differs") &&
         check(list.find("scalar") != std::string::npos &&
                   list.find("baseline") != std::string::npos &&
                   list.find("test SIMD ISA") != std::string::npos &&
                   list.find("available") != std::string::npos,
               "formatted DSP backend list differs");
}

static bool testRejectedChange(
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto saved = pipetune::saveDspBackendKind(
      configPath, pipetune::DspBackendKind::scalar);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = true;
  }
  const auto changed = pipetune::executeSetDspBackend(
      {.configPath = configPath, .socketPath = socketPath},
      pipetune::DspBackendKind::simd);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = false;
  }
  return check(saved.empty(), saved) &&
         check(!changed.success && !changed.liveApplied &&
                   !changed.persistenceApplied,
               "daemon rejection must not persist a DSP backend") &&
         configHasBackendSelection(
             configPath, pipetune::DspBackendKind::scalar);
}

static bool testSuccessfulAndPartialChanges(
    const std::filesystem::path &directory,
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto changed = pipetune::executeSetDspBackend(
      {.configPath = configPath, .socketPath = socketPath},
      pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::x86_64_v3);
  if (!check(changed.success && changed.liveApplied &&
                 changed.persistenceApplied,
             changed.error) ||
      !check(changed.status.configuredDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 changed.status.effectiveDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 changed.status.configuredDspSimdVariant ==
                     pipetune::DspSimdVariant::x86_64_v3 &&
                 changed.status.effectiveDspVariant ==
                     pipetune::DspBackendVariant::x86_64_v3,
             "live DSP backend confirmation differs") ||
      !configHasBackendSelection(
          configPath, pipetune::DspBackendKind::simd,
          pipetune::DspSimdVariant::x86_64_v3)) {
    return false;
  }

  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  const auto partial = pipetune::executeSetDspBackend(
      {.configPath = blockedParent / "environment",
       .socketPath = socketPath},
      pipetune::DspBackendKind::scalar);
  auto requestCount = std::size_t{0};
  {
    auto lock = std::scoped_lock(state.mutex);
    requestCount = state.setRequests;
  }
  return check(!partial.success && partial.liveApplied &&
                   !partial.persistenceApplied,
               "DSP persistence failure must report partial live success") &&
         check(partial.error.find("applied live") != std::string::npos,
               "partial DSP backend success must be explicit") &&
         check(requestCount == 2,
               "daemon DSP backend request count differs");
}

static bool testOfflineChange(
    const std::filesystem::path &configPath,
    const std::filesystem::path &missingSocket) {
  const auto changed = pipetune::executeSetDspBackend(
      {.configPath = configPath, .socketPath = missingSocket},
      pipetune::DspBackendKind::scalar);
  return check(changed.success && !changed.liveApplied &&
                   changed.persistenceApplied,
               "offline DSP backend set must persist for the next start") &&
         check(!changed.notice.empty(),
               "offline DSP backend set must explain deferred live application") &&
         configHasBackendSelection(
             configPath, pipetune::DspBackendKind::scalar);
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-dsp-command-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto socketPath = directory / "control.sock";
  auto state = ServerState{
      .mutex = {},
      .rejectChanges = false,
      .backend = pipetune::DspBackendKind::scalar,
      .simdVariant = pipetune::DspSimdVariant::automatic,
      .setRequests = 0,
  };
  auto started = pipetune::startControlServer(
      socketPath,
      {.handler = handleRequest,
       .statusProvider = provideStatus,
       .userData = &state});
  if (!check(started.server != nullptr, started.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto passed =
      testStatusAndFormatting(socketPath) &&
      testRejectedChange(configPath, socketPath, state) &&
      testSuccessfulAndPartialChanges(directory, configPath, socketPath,
                                      state);
  started.server.reset();
  passed =
      passed &&
      testOfflineChange(configPath, directory / "missing-control.sock");
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
