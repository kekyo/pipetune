#include "output_command.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <unistd.h>

struct ServerState {
  std::mutex mutex;
  bool rejectChanges;
  std::string preferredTarget;
  std::string selectedTarget;
  pipetune::ControlOutputSelectionReason reason;
  std::size_t setRequests;
  std::size_t clearRequests;
};

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlRuntimeStatus serverStatus(ServerState &state) {
  auto lock = std::scoped_lock(state.mutex);
  auto outputs = std::vector<pipetune::ControlOutputDevice>{
      {.name = "alsa_output.speaker",
       .description = "Built-in Speakers",
       .systemDefault = true,
       .preferred = state.preferredTarget == "alsa_output.speaker",
       .selected = state.selectedTarget == "alsa_output.speaker"},
      {.name = "alsa_output.headphones",
       .description = "USB Headphones",
       .systemDefault = false,
       .preferred = state.preferredTarget == "alsa_output.headphones",
       .selected = state.selectedTarget == "alsa_output.headphones"}};
  return {.processingMode = pipetune::ProcessingMode::bypass,
          .activePreset = {},
          .configurationError = {},
          .activePluginCount = 0,
          .preferredTarget = state.preferredTarget,
          .selectedTarget = state.selectedTarget,
          .outputSelectionReason = state.reason,
          .availableOutputs = std::move(outputs),
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
  if (request.request.command == pipetune::ControlCommand::status) {
    return {.response =
                pipetune::makeControlSuccessResponse(serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }

  {
    auto lock = std::scoped_lock(state.mutex);
    if (state.rejectChanges) {
      return {.response =
                  pipetune::makeControlErrorResponse("output change rejected"),
              .connectionMode = pipetune::ControlConnectionMode::close,
              .publishStatus = false};
    }
    if (request.request.command == pipetune::ControlCommand::setOutput) {
      ++state.setRequests;
      state.preferredTarget = request.request.outputTarget;
      if (state.preferredTarget == "alsa_output.speaker" ||
          state.preferredTarget == "alsa_output.headphones") {
        state.selectedTarget = state.preferredTarget;
        state.reason = pipetune::ControlOutputSelectionReason::preferred;
      } else {
        state.selectedTarget = "alsa_output.speaker";
        state.reason = pipetune::ControlOutputSelectionReason::fallback;
      }
    } else if (request.request.command ==
               pipetune::ControlCommand::clearOutput) {
      ++state.clearRequests;
      state.preferredTarget.clear();
      state.selectedTarget = "alsa_output.speaker";
      state.reason =
          pipetune::ControlOutputSelectionReason::systemDefault;
    } else {
      return {.response =
                  pipetune::makeControlErrorResponse("unexpected command"),
              .connectionMode = pipetune::ControlConnectionMode::close,
              .publishStatus = false};
    }
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = true};
}

static bool configHasOutput(const std::filesystem::path &configPath,
                            bool expectedFound,
                            std::string_view expectedTarget) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.config.preferredOutputFound == expectedFound,
               "stored output-presence state differs") &&
         check(loaded.config.preferredOutput == expectedTarget,
               "stored output target differs");
}

static bool testStatusAndFormatting(
    const std::filesystem::path &socketPath) {
  const auto queried = pipetune::queryOutputStatus(socketPath);
  if (!check(queried.success, queried.error) ||
      !check(queried.status.availableOutputs.size() == 2,
             "output query did not return both devices") ||
      !check(!queried.json.empty(),
             "output query must preserve JSON for --json")) {
    return false;
  }
  const auto list = pipetune::formatOutputDeviceList(queried.status);
  const auto selection = pipetune::formatOutputSelection(queried.status);
  return check(list.find("Built-in Speakers") != std::string::npos &&
                   list.find("alsa_output.speaker") != std::string::npos &&
                   list.find("USB Headphones") != std::string::npos,
               "human-readable list omits output identity") &&
         check(selection.find("System default") != std::string::npos &&
                   selection.find("alsa_output.speaker") != std::string::npos,
               "human-readable selection omits effective output state");
}

static bool testInteractiveSelection(
    const std::filesystem::path &socketPath) {
  const auto queried = pipetune::queryOutputStatus(socketPath);
  if (!check(queried.success, queried.error)) {
    return false;
  }

  auto selectInput = std::istringstream{"bad\n9\n2\n"};
  auto selectOutput = std::ostringstream{};
  const auto selected = pipetune::promptForOutputSelection(
      queried.status, selectInput, selectOutput);
  auto clearInput = std::istringstream{"0\n"};
  auto clearOutput = std::ostringstream{};
  const auto cleared = pipetune::promptForOutputSelection(
      queried.status, clearInput, clearOutput);
  auto cancelledInput = std::istringstream{};
  auto cancelledOutput = std::ostringstream{};
  const auto cancelled = pipetune::promptForOutputSelection(
      queried.status, cancelledInput, cancelledOutput);
  return check(selected.success && !selected.clearPreference &&
                   selected.target == "alsa_output.headphones",
               "interactive device choice differs") &&
         check(selectOutput.str().find("Invalid selection") !=
                   std::string::npos,
               "interactive selection must explain invalid input") &&
         check(cleared.success && cleared.clearPreference &&
                   cleared.target.empty(),
               "interactive system-default choice differs") &&
         check(!cancelled.success && !cancelled.error.empty(),
               "interactive EOF must cancel selection");
}

static bool testDisconnectedSet(
    const std::filesystem::path &configPath,
    const std::filesystem::path &missingSocket) {
  const auto saved =
      pipetune::savePreferredOutput(configPath, "alsa_output.previous");
  if (!check(saved.empty(), saved)) {
    return false;
  }
  const auto result = pipetune::executeSetPreferredOutput(
      {.configPath = configPath, .socketPath = missingSocket},
      "alsa_output.headphones");
  return check(!result.success,
               "daemon absence must fail output set") &&
         check(!result.liveApplied && !result.persistenceApplied,
               "daemon absence must not change either output phase") &&
         check(!result.error.empty(),
               "daemon absence must report a diagnostic") &&
         configHasOutput(configPath, true, "alsa_output.previous");
}

static bool testRejectedSet(const std::filesystem::path &configPath,
                            const std::filesystem::path &socketPath,
                            ServerState &state) {
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = true;
  }
  const auto result = pipetune::executeSetPreferredOutput(
      {.configPath = configPath, .socketPath = socketPath},
      "alsa_output.headphones");
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = false;
  }
  return check(!result.success,
               "rejected live output set must fail") &&
         check(!result.liveApplied && !result.persistenceApplied,
               "rejected live output set must not persist") &&
         configHasOutput(configPath, true, "alsa_output.previous");
}

static bool testSuccessfulChanges(
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto set = pipetune::executeSetPreferredOutput(
      {.configPath = configPath, .socketPath = socketPath},
      "alsa_output.headphones");
  if (!check(set.success && set.liveApplied && set.persistenceApplied,
             set.error) ||
      !configHasOutput(configPath, true, "alsa_output.headphones")) {
    return false;
  }

  const auto fallback = pipetune::executeSetPreferredOutput(
      {.configPath = configPath, .socketPath = socketPath},
      "alsa_output.disconnected-usb");
  if (!check(fallback.success && fallback.status.preferredTarget ==
                                      "alsa_output.disconnected-usb" &&
                 fallback.status.outputSelectionReason ==
                     pipetune::ControlOutputSelectionReason::fallback,
             "an unavailable preference must be accepted while falling back") ||
      !configHasOutput(configPath, true,
                       "alsa_output.disconnected-usb")) {
    return false;
  }

  const auto clear = pipetune::executeClearPreferredOutput(
      {.configPath = configPath, .socketPath = socketPath});
  auto requestCountsValid = false;
  {
    auto lock = std::scoped_lock(state.mutex);
    requestCountsValid =
        state.setRequests == 2 && state.clearRequests == 1;
  }
  return check(clear.success && clear.liveApplied &&
                   clear.persistenceApplied,
               clear.error) &&
         check(clear.status.preferredTarget.empty() &&
                   clear.status.outputSelectionReason ==
                       pipetune::ControlOutputSelectionReason::systemDefault,
               "clear must make the daemon follow the system default") &&
         check(requestCountsValid,
               "server output-change request counts differ") &&
         configHasOutput(configPath, false, {});
}

static bool testPersistenceFailureAfterLiveSet(
    const std::filesystem::path &directory,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  const auto result = pipetune::executeSetPreferredOutput(
      {.configPath = blockedParent / "environment",
       .socketPath = socketPath},
      "alsa_output.headphones");
  auto liveTarget = std::string{};
  {
    auto lock = std::scoped_lock(state.mutex);
    liveTarget = state.preferredTarget;
  }
  return check(!result.success,
               "persistence failure must fail output set") &&
         check(result.liveApplied && !result.persistenceApplied,
               "persistence failure must preserve partial-success phases") &&
         check(liveTarget == "alsa_output.headphones",
               "live output must change before persistence") &&
         check(result.error.find("applied live") != std::string::npos,
               "partial success must be explicit");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-output-command-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto socketPath = directory / "control.sock";

  auto state = ServerState{
      .mutex = {},
      .rejectChanges = false,
      .preferredTarget = {},
      .selectedTarget = "alsa_output.speaker",
      .reason = pipetune::ControlOutputSelectionReason::systemDefault,
      .setRequests = 0,
      .clearRequests = 0,
  };
  auto passed =
      testDisconnectedSet(configPath, directory / "missing.sock");
  auto started = pipetune::startControlServer(
      socketPath,
      {.handler = handleRequest,
       .statusProvider = provideStatus,
       .userData = &state});
  if (!check(started.server != nullptr, started.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }
  passed = passed && testStatusAndFormatting(socketPath) &&
           testInteractiveSelection(socketPath) &&
           testRejectedSet(configPath, socketPath, state) &&
           testSuccessfulChanges(configPath, socketPath, state) &&
           testPersistenceFailureAfterLiveSet(
               directory, socketPath, state);
  started.server.reset();
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
