#include "bypass_command.h"

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
  bool rejectBypass;
  bool bypassed;
  std::size_t bypassRequests;
};

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlRuntimeStatus serverStatus(ServerState &state) {
  auto lock = std::scoped_lock(state.mutex);
  return {.processingMode = state.bypassed
                                ? pipetune::ProcessingMode::bypass
                                : pipetune::ProcessingMode::preset,
          .activePreset =
              state.bypassed ? std::string{}
                             : std::string("/tmp/active.effetune_preset"),
          .configurationError = {},
          .activePluginCount = state.bypassed ? 0u : 1u,
          .preferredTarget = {},
          .selectedTarget = "alsa_output.test",
          .outputSelectionReason =
              pipetune::ControlOutputSelectionReason::systemDefault,
          .availableOutputs =
              {{.name = "alsa_output.test",
                .description = "Test Output",
                .systemDefault = true,
                .preferred = false,
                .selected = true}},
          .defaultSinkActive = true,
          .overrunFrames = 0,
          .underrunFrames = 0,
          .processingErrors = 0,
          .dspProcessedFrames = 0,
          .dspProcessingNanoseconds = 0,
          .inputSampleFormat = {},
          .inputSampleRate = 0,
          .inputChannelCount = 0,
          .inputFramesReceived = 0,
          .inputLastReceivedUnixMilliseconds = 0};
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
  if (request.request.command != pipetune::ControlCommand::bypass) {
    return {.response =
                pipetune::makeControlSuccessResponse(serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }

  {
    auto lock = std::scoped_lock(state.mutex);
    ++state.bypassRequests;
    if (state.rejectBypass) {
      return {.response =
                  pipetune::makeControlErrorResponse("bypass rejected"),
              .connectionMode = pipetune::ControlConnectionMode::close,
              .publishStatus = false};
    }
    state.bypassed = true;
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = true};
}

static bool configContainsPreset(const std::filesystem::path &configPath,
                                 bool expected) {
  const auto loaded = pipetune::loadStartupPreset(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.found == expected,
               expected ? "startup preset was unexpectedly cleared"
                        : "startup preset was not cleared");
}

static bool testDisconnectedDaemon(
    const std::filesystem::path &configPath,
    const std::filesystem::path &missingSocket) {
  const auto saved = pipetune::saveStartupPreset(
      configPath, "/tmp/saved.effetune_preset");
  if (!check(saved.empty(), saved)) {
    return false;
  }
  const auto result = pipetune::executePersistentBypass(
      {.configPath = configPath, .socketPath = missingSocket});
  return check(result.success,
               "daemon absence must not prevent persistent bypass") &&
         check(!result.liveApplied,
               "daemon absence must not report a live change") &&
         check(result.persistenceApplied,
               "daemon absence must still clear startup processing") &&
         check(!result.notice.empty(),
               "deferred bypass must explain that it applies next start") &&
         configContainsPreset(configPath, false);
}

static bool testRejectedLiveBypass(const std::filesystem::path &configPath,
                                   const std::filesystem::path &socketPath,
                                   ServerState &state) {
  const auto saved = pipetune::saveStartupPreset(
      configPath, "/tmp/saved.effetune_preset");
  if (!check(saved.empty(), saved)) {
    return false;
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectBypass = true;
    state.bypassed = false;
  }
  const auto result = pipetune::executePersistentBypass(
      {.configPath = configPath, .socketPath = socketPath});
  return check(!result.success, "a rejected live bypass must fail") &&
         check(!result.liveApplied,
               "a rejected live bypass must not report live success") &&
         check(!result.persistenceApplied,
               "a rejected live bypass must preserve startup processing") &&
         check(!result.error.empty(),
               "a rejected live bypass must report its diagnostic") &&
         configContainsPreset(configPath, true);
}

static bool testSuccessfulBypass(const std::filesystem::path &configPath,
                                 const std::filesystem::path &socketPath,
                                 ServerState &state) {
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectBypass = false;
    state.bypassed = false;
  }
  const auto result = pipetune::executePersistentBypass(
      {.configPath = configPath, .socketPath = socketPath});
  auto bypassed = false;
  {
    auto lock = std::scoped_lock(state.mutex);
    bypassed = state.bypassed;
  }
  return check(result.success, result.error) &&
         check(result.liveApplied,
               "connected bypass must change the live daemon") &&
         check(result.persistenceApplied,
               "connected bypass must clear startup processing") &&
         check(bypassed, "server did not receive the bypass request") &&
         configContainsPreset(configPath, false);
}

static bool testPersistenceFailureAfterLiveBypass(
    const std::filesystem::path &directory,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectBypass = false;
    state.bypassed = false;
  }
  const auto result = pipetune::executePersistentBypass(
      {.configPath = blockedParent / "environment",
       .socketPath = socketPath});
  auto bypassed = false;
  {
    auto lock = std::scoped_lock(state.mutex);
    bypassed = state.bypassed;
  }
  return check(!result.success,
               "startup persistence failure must fail the command") &&
         check(result.liveApplied,
               "persistence failure must retain live bypass success") &&
         check(!result.persistenceApplied,
               "failed persistence must not report success") &&
         check(bypassed,
               "live bypass must occur before startup persistence") &&
         check(!result.error.empty(),
               "partial bypass failure must report its diagnostic");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-bypass-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto socketPath = directory / "control.sock";

  auto state = ServerState{.mutex = {},
                           .rejectBypass = false,
                           .bypassed = false,
                           .bypassRequests = 0};
  auto passed =
      testDisconnectedDaemon(configPath, directory / "missing.sock");
  auto started = pipetune::startControlServer(
      socketPath,
      {.handler = handleRequest,
       .statusProvider = provideStatus,
       .userData = &state});
  if (!check(started.server != nullptr, started.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }
  passed =
      passed && testRejectedLiveBypass(configPath, socketPath, state) &&
      testSuccessfulBypass(configPath, socketPath, state) &&
      testPersistenceFailureAfterLiveBypass(directory, socketPath, state);
  started.server.reset();
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
