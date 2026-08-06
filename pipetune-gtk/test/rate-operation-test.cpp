#include "application-state.h"
#include "control-client.h"
#include "rate-operation.h"

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

static pipetune::SampleRatePolicy fixedPolicy(
    std::uint32_t rate,
    pipetune::SampleRateEnforcement enforcement) {
  return {.mode = pipetune::SampleRateMode::fixed,
          .fixedRate = rate,
          .enforcement = enforcement};
}

static pipetune::ControlRuntimeStatus rateStatus(
    const pipetune::SampleRatePolicy &policy) {
  return {.processingMode = pipetune::ProcessingMode::bypass,
          .activePreset = {},
          .configurationError = {},
          .activePluginCount = 0,
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
          .configuredRatePolicy = policy,
          .dspSampleRate =
              policy.mode == pipetune::SampleRateMode::automatic
                  ? 96000u
                  : policy.fixedRate,
          .graphSampleRate =
              policy.mode == pipetune::SampleRateMode::automatic
                  ? 96000u
                  : policy.fixedRate,
          .rateTransitioning = false,
          .rateError = {}};
}

static pipetune_gtk::ApplicationState pendingState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime = rateStatus(pipetune::defaultSampleRatePolicy());
  state.operationPending = true;
  return state;
}

static pipetune_gtk::ControlClientReply successfulReply(
    const pipetune::SampleRatePolicy &policy) {
  return {
      .response = pipetune::parseControlResponse(
          pipetune::makeControlSuccessResponse(rateStatus(policy), {})),
      .transportError = {},
  };
}

static bool configHasPolicy(
    const std::filesystem::path &configPath,
    const pipetune::SampleRatePolicy &expected) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.config.ratePolicy == expected,
               "GTK stored sample-rate policy differs");
}

static bool testRejectedAndUnconfirmedReplies(
    const std::filesystem::path &configPath) {
  const auto oldPolicy = pipetune::defaultSampleRatePolicy();
  const auto requested =
      fixedPolicy(192000, pipetune::SampleRateEnforcement::force);
  if (!check(
          pipetune::saveSampleRatePolicy(configPath, oldPolicy).empty(),
          "cannot seed GTK rate-operation configuration")) {
    return false;
  }

  auto rejectedState = pendingState();
  const auto rejected = pipetune_gtk::completeRateOperation(
      rejectedState,
      {.response = pipetune::parseControlResponse(
           pipetune::makeControlErrorResponse("rate rejected")),
       .transportError = {}},
      {.configPath = configPath, .policy = requested}, 1000);
  if (!check(!rejected.liveApplied && !rejected.persistenceApplied &&
                 !rejected.reconnectRequired,
             "rejected GTK rate operation phases differ") ||
      !check(!rejectedState.operationPending,
             "rejected GTK rate operation must leave pending mode") ||
      !configHasPolicy(configPath, oldPolicy)) {
    return false;
  }

  auto unconfirmedState = pendingState();
  auto reply = successfulReply(requested);
  reply.response.status.rateTransitioning = true;
  const auto unconfirmed = pipetune_gtk::completeRateOperation(
      unconfirmedState, reply,
      {.configPath = configPath, .policy = requested}, 2000);
  return check(!unconfirmed.liveApplied &&
                   !unconfirmed.persistenceApplied,
               "transitioning reply must not confirm a live rate") &&
         check(unconfirmedState.diagnostic.find("did not confirm") !=
                   std::string::npos,
               "unconfirmed live rate must explain the failure") &&
         configHasPolicy(configPath, oldPolicy);
}

static bool testSuccessfulAndOfflineChanges(
    const std::filesystem::path &configPath) {
  const auto requested =
      fixedPolicy(192000, pipetune::SampleRateEnforcement::force);
  auto state = pendingState();
  const auto result = pipetune_gtk::completeRateOperation(
      state, successfulReply(requested),
      {.configPath = configPath, .policy = requested}, 3000);
  if (!check(result.liveApplied && result.persistenceApplied &&
                 !result.reconnectRequired,
             "successful GTK rate operation phases differ") ||
      !check(state.runtime.configuredRatePolicy == requested &&
                 state.runtime.dspSampleRate == 192000 &&
                 !state.operationPending,
             "successful GTK rate operation state differs") ||
      !configHasPolicy(configPath, requested)) {
    return false;
  }

  const auto offlinePolicy =
      fixedPolicy(44100, pipetune::SampleRateEnforcement::suggest);
  auto offlineState = pipetune_gtk::initialApplicationState();
  const auto offline = pipetune_gtk::persistRateOperationForNextStart(
      offlineState,
      {.configPath = configPath, .policy = offlinePolicy});
  return check(!offline.liveApplied && offline.persistenceApplied &&
                   !offline.reconnectRequired,
               "offline GTK rate persistence phases differ") &&
         check(offlineState.diagnostic.find("next start") !=
                   std::string::npos,
               "offline GTK rate persistence must be explicit") &&
         configHasPolicy(configPath, offlinePolicy);
}

static bool testDisconnectPersistsForNextStart(
    const std::filesystem::path &configPath) {
  const auto requested =
      fixedPolicy(384000, pipetune::SampleRateEnforcement::suggest);
  auto state = pendingState();
  const auto result = pipetune_gtk::completeRateOperation(
      state, {.response = {}, .transportError = "daemon disconnected"},
      {.configPath = configPath, .policy = requested}, 4000);
  return check(result.reconnectRequired && !result.liveApplied &&
                   result.persistenceApplied,
               "disconnected GTK rate operation phases differ") &&
         check(state.connection ==
                   pipetune_gtk::ControlConnectionState::disconnected,
               "rate transport failure must disconnect the GTK state") &&
         check(state.diagnostic.find("next start") != std::string::npos,
               "rate transport failure must report offline persistence") &&
         configHasPolicy(configPath, requested);
}

static bool testPersistenceFailure(
    const std::filesystem::path &directory) {
  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  const auto requested =
      fixedPolicy(96000, pipetune::SampleRateEnforcement::force);
  auto state = pendingState();
  const auto result = pipetune_gtk::completeRateOperation(
      state, successfulReply(requested),
      {.configPath = blockedParent / "environment", .policy = requested},
      5000);
  return check(result.liveApplied && !result.persistenceApplied,
               "GTK partial rate success phases differ") &&
         check(state.diagnostic.find("applied live") != std::string::npos,
               "GTK partial rate success must be explicit");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-gtk-rate-operation-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto passed =
      testRejectedAndUnconfirmedReplies(configPath) &&
      testSuccessfulAndOfflineChanges(configPath) &&
      testDisconnectPersistsForNextStart(configPath) &&
      testPersistenceFailure(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
