#include "application-state.h"
#include "control-client.h"
#include "output-operation.h"

#include "pipetune/control_protocol.h"
#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlRuntimeStatus outputStatus(
    std::string preferredTarget) {
  const auto preferred = !preferredTarget.empty();
  const auto selected =
      preferred ? preferredTarget : std::string("alsa_output.speaker");
  return {.processingMode = pipetune::ProcessingMode::bypass,
          .activePreset = {},
          .configurationError = {},
          .activePluginCount = 0,
          .preferredTarget = std::move(preferredTarget),
          .selectedTarget = selected,
          .outputSelectionReason =
              preferred
                  ? pipetune::ControlOutputSelectionReason::preferred
                  : pipetune::ControlOutputSelectionReason::systemDefault,
          .availableOutputs =
              {{.name = "alsa_output.speaker",
                .description = "Speakers",
                .systemDefault = true,
                .preferred = false,
                .selected = !preferred},
               {.name = "alsa_output.headphones",
                .description = "Headphones",
                .systemDefault = false,
                .preferred = preferred,
                .selected = preferred}},
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

static pipetune_gtk::ApplicationState existingState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime = outputStatus("alsa_output.headphones");
  state.operationPending = true;
  return state;
}

static pipetune_gtk::ControlClientReply successfulReply(
    std::string preferredTarget) {
  return {
      .response = pipetune::parseControlResponse(
          pipetune::makeControlSuccessResponse(
              outputStatus(std::move(preferredTarget)), {})),
      .transportError = {},
  };
}

static bool configHasOutput(const std::filesystem::path &configPath,
                            bool expectedFound,
                            std::string_view expectedTarget) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.config.preferredOutputFound == expectedFound,
               "GTK stored output-presence state differs") &&
         check(loaded.config.preferredOutput == expectedTarget,
               "GTK stored output target differs");
}

static bool testRejectedAndDisconnectedChanges(
    const std::filesystem::path &configPath) {
  const auto saved =
      pipetune::savePreferredOutput(configPath, "alsa_output.headphones");
  if (!check(saved.empty(), saved)) {
    return false;
  }

  auto rejectedState = existingState();
  const auto rejected = pipetune_gtk::completeOutputOperation(
      rejectedState,
      {.response = pipetune::parseControlResponse(
           pipetune::makeControlErrorResponse("output rejected")),
       .transportError = {}},
      {.configPath = configPath,
       .clearPreference = false,
       .target = "alsa_output.speaker"},
      1000);
  if (!check(!rejected.liveApplied && !rejected.persistenceApplied &&
                 !rejected.reconnectRequired,
             "rejected GTK output operation phases differ") ||
      !check(!rejectedState.operationPending &&
                 rejectedState.runtime.preferredTarget ==
                     "alsa_output.headphones",
             "rejected GTK output operation must restore engine state") ||
      !configHasOutput(configPath, true, "alsa_output.headphones")) {
    return false;
  }

  auto disconnectedState = existingState();
  const auto disconnected = pipetune_gtk::completeOutputOperation(
      disconnectedState,
      {.response = {}, .transportError = "daemon disconnected"},
      {.configPath = configPath,
       .clearPreference = true,
       .target = {}},
      2000);
  return check(disconnected.reconnectRequired &&
                   !disconnected.liveApplied &&
                   !disconnected.persistenceApplied,
               "disconnected GTK output operation phases differ") &&
         check(disconnectedState.connection ==
                   pipetune_gtk::ControlConnectionState::disconnected &&
                   disconnectedState.runtime.preferredTarget ==
                       "alsa_output.headphones",
               "disconnect must retain the last engine output state") &&
         configHasOutput(configPath, true, "alsa_output.headphones");
}

static bool testSuccessfulChanges(
    const std::filesystem::path &configPath) {
  auto setState = existingState();
  const auto set = pipetune_gtk::completeOutputOperation(
      setState, successfulReply("alsa_output.headphones"),
      {.configPath = configPath,
       .clearPreference = false,
       .target = "alsa_output.headphones"},
      3000);
  if (!check(set.liveApplied && set.persistenceApplied &&
                 !set.reconnectRequired,
             "successful GTK output set phases differ") ||
      !check(setState.runtime.preferredTarget ==
                 "alsa_output.headphones" &&
                 !setState.operationPending,
             "successful GTK output set state differs") ||
      !configHasOutput(configPath, true, "alsa_output.headphones")) {
    return false;
  }

  auto clearState = existingState();
  const auto clear = pipetune_gtk::completeOutputOperation(
      clearState, successfulReply({}),
      {.configPath = configPath,
       .clearPreference = true,
       .target = {}},
      4000);
  return check(clear.liveApplied && clear.persistenceApplied,
               "successful GTK output clear phases differ") &&
         check(clearState.runtime.preferredTarget.empty(),
               "successful GTK output clear state differs") &&
         configHasOutput(configPath, false, {});
}

static bool testPersistenceFailure(
    const std::filesystem::path &directory) {
  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  auto state = existingState();
  const auto result = pipetune_gtk::completeOutputOperation(
      state, successfulReply("alsa_output.headphones"),
      {.configPath = blockedParent / "environment",
       .clearPreference = false,
       .target = "alsa_output.headphones"},
      5000);
  return check(result.liveApplied && !result.persistenceApplied,
               "GTK partial output success phases differ") &&
         check(state.runtime.preferredTarget ==
                   "alsa_output.headphones",
               "GTK persistence failure must retain live engine state") &&
         check(state.diagnostic.find("applied live") != std::string::npos,
               "GTK partial output success must be explicit");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-gtk-output-operation-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto passed =
      testRejectedAndDisconnectedChanges(configPath) &&
      testSuccessfulChanges(configPath) &&
      testPersistenceFailure(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
