#include "idle_command.h"

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
  pipetune::DspIdlePolicy policy;
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
  auto status = pipetune::ControlRuntimeStatus{
      .processingMode = pipetune::ProcessingMode::bypass,
      .activePreset = {},
      .configurationError = {},
      .activePluginCount = 0,
      .preferredTarget = {},
      .selectedTarget = {},
      .outputSelectionReason =
          pipetune::ControlOutputSelectionReason::unavailable,
      .availableOutputs = {},
      .defaultSinkActive = true,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 48000,
      .dspProcessingNanoseconds = 1000000,
      .inputSampleFormat = "F32P",
      .inputSampleRate = 48000,
      .inputChannelCount = 2,
      .inputFramesReceived = 96000,
      .inputLastReceivedUnixMilliseconds = 1720000000000};
  status.dspIdlePolicy = state.policy;
  status.dspIdleState = pipetune::DspIdleState::sleeping;
  status.dspIdleSkippedFrames = 48000;
  status.dspIdleSleepTransitions = 2;
  status.pipeWireIdle = true;
  return status;
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
      pipetune::ControlCommand::setDspIdlePolicy) {
    return {.response =
                pipetune::makeControlErrorResponse("unexpected command"),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    if (state.rejectChanges) {
      return {
          .response =
              pipetune::makeControlErrorResponse("idle policy rejected"),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = false};
    }
    state.policy = request.request.dspIdlePolicy;
    ++state.setRequests;
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = true};
}

static bool configHasPolicy(
    const std::filesystem::path &configPath,
    pipetune::DspIdlePolicy expected) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.dspIdlePolicy == expected,
               "stored DSP idle policy differs");
}

static bool testStatusAndFormatting(
    const std::filesystem::path &socketPath) {
  const auto queried = pipetune::queryIdleStatus(socketPath);
  if (!check(queried.success, queried.error) ||
      !check(!queried.json.empty(), "idle query must preserve JSON")) {
    return false;
  }
  const auto formatted = pipetune::formatIdleStatus(queried.status);
  return check(formatted.find("conservative") != std::string::npos &&
                   formatted.find("sleeping") != std::string::npos &&
                   formatted.find("48,000") != std::string::npos &&
                   formatted.find("paused") != std::string::npos,
               "formatted idle status omits policy, state, or counters");
}

static bool testRejectedChange(
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto saved = pipetune::saveDspIdlePolicy(
      configPath, pipetune::DspIdlePolicy::conservative);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = true;
  }
  const auto result = pipetune::executeSetDspIdlePolicy(
      {.configPath = configPath, .socketPath = socketPath},
      pipetune::DspIdlePolicy::exact);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = false;
  }
  return check(saved.empty(), saved) &&
         check(!result.success && !result.liveApplied &&
                   !result.persistenceApplied,
               "daemon rejection must not persist an idle policy") &&
         configHasPolicy(configPath,
                         pipetune::DspIdlePolicy::conservative);
}

static bool testSuccessfulAndPartialChanges(
    const std::filesystem::path &directory,
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto changed = pipetune::executeSetDspIdlePolicy(
      {.configPath = configPath, .socketPath = socketPath},
      pipetune::DspIdlePolicy::exact);
  if (!check(changed.success && changed.liveApplied &&
                 changed.persistenceApplied,
             changed.error) ||
      !check(changed.status.dspIdlePolicy ==
                 pipetune::DspIdlePolicy::exact,
             "live daemon confirmation differs") ||
      !configHasPolicy(configPath, pipetune::DspIdlePolicy::exact)) {
    return false;
  }

  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  const auto partial = pipetune::executeSetDspIdlePolicy(
      {.configPath = blockedParent / "environment",
       .socketPath = socketPath},
      pipetune::DspIdlePolicy::conservative);
  auto requestCount = std::size_t{0};
  {
    auto lock = std::scoped_lock(state.mutex);
    requestCount = state.setRequests;
  }
  return check(!partial.success && partial.liveApplied &&
                   !partial.persistenceApplied,
               "persistence failure must report partial live success") &&
         check(partial.error.find("applied live") != std::string::npos,
               "partial idle-policy success must be explicit") &&
         check(requestCount == 2, "daemon idle request count differs");
}

static bool testOfflineChange(
    const std::filesystem::path &configPath,
    const std::filesystem::path &missingSocket) {
  const auto changed = pipetune::executeSetDspIdlePolicy(
      {.configPath = configPath, .socketPath = missingSocket},
      pipetune::DspIdlePolicy::exact);
  return check(changed.success && !changed.liveApplied &&
                   changed.persistenceApplied,
               "offline idle set must persist for the next daemon start") &&
         check(!changed.notice.empty(),
               "offline idle set must explain deferred live application") &&
         configHasPolicy(configPath, pipetune::DspIdlePolicy::exact);
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-idle-command-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto socketPath = directory / "control.sock";
  auto state = ServerState{
      .mutex = {},
      .rejectChanges = false,
      .policy = pipetune::DspIdlePolicy::conservative,
      .setRequests = 0};
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
      testSuccessfulAndPartialChanges(
          directory, configPath, socketPath, state);
  started.server.reset();
  passed =
      passed &&
      testOfflineChange(configPath, directory / "missing-control.sock");
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
