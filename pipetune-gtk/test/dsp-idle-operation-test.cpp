#include "application-state.h"
#include "control-client.h"
#include "dsp-idle-operation.h"

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

static pipetune::ControlRuntimeStatus idleStatus(
    pipetune::DspIdlePolicy policy) {
  auto status = pipetune::ControlRuntimeStatus{};
  status.dspIdlePolicy = policy;
  status.dspIdleState = pipetune::DspIdleState::active;
  return status;
}

static pipetune_gtk::ApplicationState pendingState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime = idleStatus(pipetune::DspIdlePolicy::conservative);
  state.operationPending = true;
  return state;
}

static pipetune_gtk::ControlClientReply successfulReply(
    pipetune::DspIdlePolicy policy) {
  return {
      .response = pipetune::parseControlResponse(
          pipetune::makeControlSuccessResponse(idleStatus(policy), {})),
      .transportError = {},
  };
}

static bool configHasPolicy(
    const std::filesystem::path &configPath,
    pipetune::DspIdlePolicy expected) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.dspIdlePolicy == expected,
               "GTK stored DSP idle policy differs");
}

static bool testRejectedAndUnconfirmedReplies(
    const std::filesystem::path &configPath) {
  if (!check(pipetune::saveDspIdlePolicy(
                 configPath, pipetune::DspIdlePolicy::conservative)
                 .empty(),
             "cannot seed GTK DSP idle configuration")) {
    return false;
  }

  auto rejectedState = pendingState();
  const auto rejected = pipetune_gtk::completeDspIdleOperation(
      rejectedState,
      {.response = pipetune::parseControlResponse(
           pipetune::makeControlErrorResponse("idle policy rejected")),
       .transportError = {}},
      {.configPath = configPath,
       .policy = pipetune::DspIdlePolicy::exact},
      1000);
  if (!check(!rejected.liveApplied && !rejected.persistenceApplied &&
                 !rejected.reconnectRequired,
             "rejected GTK DSP idle operation phases differ") ||
      !check(!rejectedState.operationPending,
             "rejected GTK DSP idle operation must leave pending mode") ||
      !configHasPolicy(configPath,
                       pipetune::DspIdlePolicy::conservative)) {
    return false;
  }

  auto unconfirmedState = pendingState();
  const auto unconfirmed = pipetune_gtk::completeDspIdleOperation(
      unconfirmedState,
      successfulReply(pipetune::DspIdlePolicy::conservative),
      {.configPath = configPath,
       .policy = pipetune::DspIdlePolicy::exact},
      2000);
  return check(!unconfirmed.liveApplied &&
                   !unconfirmed.persistenceApplied,
               "unconfirmed DSP idle reply must not persist") &&
         check(unconfirmedState.diagnostic.find("did not confirm") !=
                   std::string::npos,
               "unconfirmed DSP idle policy must explain the failure") &&
         configHasPolicy(configPath,
                         pipetune::DspIdlePolicy::conservative);
}

static bool testSuccessfulAndOfflineChanges(
    const std::filesystem::path &configPath) {
  auto state = pendingState();
  const auto changed = pipetune_gtk::completeDspIdleOperation(
      state, successfulReply(pipetune::DspIdlePolicy::exact),
      {.configPath = configPath,
       .policy = pipetune::DspIdlePolicy::exact},
      3000);
  if (!check(changed.liveApplied && changed.persistenceApplied &&
                 !changed.reconnectRequired,
             "successful GTK DSP idle operation phases differ") ||
      !check(state.runtime.dspIdlePolicy ==
                     pipetune::DspIdlePolicy::exact &&
                 !state.operationPending,
             "successful GTK DSP idle operation state differs") ||
      !configHasPolicy(configPath, pipetune::DspIdlePolicy::exact)) {
    return false;
  }

  auto offlineState = pipetune_gtk::initialApplicationState();
  const auto offline =
      pipetune_gtk::persistDspIdleOperationForNextStart(
          offlineState,
          {.configPath = configPath,
           .policy = pipetune::DspIdlePolicy::conservative});
  return check(!offline.liveApplied && offline.persistenceApplied &&
                   !offline.reconnectRequired,
               "offline GTK DSP idle persistence phases differ") &&
         check(offlineState.diagnostic.find("next start") !=
                   std::string::npos,
               "offline GTK DSP idle persistence must be explicit") &&
         configHasPolicy(configPath,
                         pipetune::DspIdlePolicy::conservative);
}

static bool testDisconnectPersistsForNextStart(
    const std::filesystem::path &configPath) {
  auto state = pendingState();
  const auto changed = pipetune_gtk::completeDspIdleOperation(
      state, {.response = {}, .transportError = "daemon disconnected"},
      {.configPath = configPath,
       .policy = pipetune::DspIdlePolicy::exact},
      4000);
  return check(changed.reconnectRequired && !changed.liveApplied &&
                   changed.persistenceApplied,
               "disconnected GTK DSP idle operation phases differ") &&
         check(state.connection ==
                   pipetune_gtk::ControlConnectionState::disconnected,
               "DSP idle transport failure must disconnect GTK state") &&
         check(state.diagnostic.find("next start") != std::string::npos,
               "DSP idle transport failure must report offline persistence") &&
         configHasPolicy(configPath, pipetune::DspIdlePolicy::exact);
}

static bool testPersistenceFailure(
    const std::filesystem::path &directory) {
  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  auto state = pendingState();
  const auto changed = pipetune_gtk::completeDspIdleOperation(
      state, successfulReply(pipetune::DspIdlePolicy::exact),
      {.configPath = blockedParent / "environment",
       .policy = pipetune::DspIdlePolicy::exact},
      5000);
  return check(changed.liveApplied && !changed.persistenceApplied,
               "GTK partial DSP idle success phases differ") &&
         check(state.diagnostic.find("applied live") != std::string::npos,
               "GTK partial DSP idle success must be explicit");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-gtk-dsp-idle-operation-test-" +
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
