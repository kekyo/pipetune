#include "rate_command.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <algorithm>
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
  pipetune::SampleRatePolicy policy;
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
  const auto fixed =
      state.policy.mode == pipetune::SampleRateMode::fixed;
  const auto dspRate = fixed ? state.policy.fixedRate : 96000U;
  const auto outputRate = std::min(dspRate, 96000U);
  return {
      .processingMode = pipetune::ProcessingMode::bypass,
      .activePreset = {},
      .configurationError = {},
      .activePluginCount = 0,
      .policyBackend = "wireplumber-0.5",
      .filterOutputs =
          {{.targetNodeName = "alsa_output.usb_dac",
            .targetDescription = "USB DAC",
            .filterNodeName = "pipetune.filter.usb_dac",
            .state = pipetune::ControlFilterState::active,
            .error = {},
            .channelCount = 2,
            .sampleRateCapabilities =
                {.known = true,
                 .constraints =
                     {{.kind = pipetune::SampleRateConstraintKind::range,
                       .minimum = 44100,
                       .maximum = 96000,
                       .step = 0}}},
            .dspSampleRate = dspRate,
            .outputSampleRate = outputRate,
            .activeOutputSampleRate = outputRate,
            .rateFallback = outputRate != dspRate,
            .latencyFrames = 64,
            .overrunFrames = 0,
            .underrunFrames = 0,
            .processingErrors = 0,
            .dspProcessedFrames = 0,
            .dspProcessingNanoseconds = 0}},
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
      .configuredRatePolicy = state.policy,
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
  if (request.request.command != pipetune::ControlCommand::setRate) {
    return {.response =
                pipetune::makeControlErrorResponse("unexpected command"),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    if (state.rejectChanges) {
      return {.response =
                  pipetune::makeControlErrorResponse("rate change rejected"),
              .connectionMode = pipetune::ControlConnectionMode::close,
              .publishStatus = false};
    }
    state.policy = request.request.ratePolicy;
    ++state.setRequests;
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = true};
}

static bool configHasPolicy(
    const std::filesystem::path &configPath,
    const pipetune::SampleRatePolicy &expected) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.config.ratePolicy == expected,
               "stored sample-rate policy differs");
}

static bool testStatusAndFormatting(
    const std::filesystem::path &socketPath) {
  const auto queried = pipetune::queryRateStatus(socketPath);
  if (!check(queried.success, queried.error) ||
      !check(!queried.json.empty(), "rate query must preserve JSON")) {
    return false;
  }
  const auto status = pipetune::formatSampleRateStatus(queried.status);
  const auto capabilities =
      pipetune::formatSampleRateCapabilities(queried.status);
  return check(status.find("Max") != std::string::npos &&
                   status.find("96 kHz") != std::string::npos &&
                   status.find("suggest") != std::string::npos,
               "formatted rate status omits policy or effective rates") &&
         check(capabilities.find("USB DAC") != std::string::npos &&
                   capabilities.find("44.1 kHz") != std::string::npos &&
                   capabilities.find("supported") != std::string::npos &&
                   capabilities.find("384 kHz") != std::string::npos &&
                   capabilities.find("unsupported") != std::string::npos,
               "formatted capabilities omit support hints");
}

static bool testRejectedChange(
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto previous = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 48000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  const auto requested = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  const auto saved =
      pipetune::saveSampleRatePolicy(configPath, previous);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = true;
  }
  const auto result = pipetune::executeSetSampleRatePolicy(
      {.configPath = configPath, .socketPath = socketPath}, requested);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectChanges = false;
  }
  return check(saved.empty(), saved) &&
         check(!result.success && !result.liveApplied &&
                   !result.persistenceApplied,
               "daemon rejection must not persist a rate policy") &&
         configHasPolicy(configPath, previous);
}

static bool testSuccessfulAndPartialChanges(
    const std::filesystem::path &directory,
    const std::filesystem::path &configPath,
    const std::filesystem::path &socketPath, ServerState &state) {
  const auto requested = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  const auto changed = pipetune::executeSetSampleRatePolicy(
      {.configPath = configPath, .socketPath = socketPath}, requested);
  if (!check(changed.success && changed.liveApplied &&
                 changed.persistenceApplied,
             changed.error) ||
      !check(changed.status.configuredRatePolicy == requested &&
                 changed.status.filterOutputs.size() == 1 &&
                 changed.status.filterOutputs[0].dspSampleRate == 192000 &&
                 changed.status.filterOutputs[0].outputSampleRate == 96000 &&
                 changed.status.filterOutputs[0].rateFallback,
             "live daemon confirmation differs") ||
      !configHasPolicy(configPath, requested)) {
    return false;
  }

  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  const auto partial = pipetune::executeSetSampleRatePolicy(
      {.configPath = blockedParent / "environment",
       .socketPath = socketPath},
      pipetune::defaultSampleRatePolicy());
  auto requestCount = std::size_t{0};
  {
    auto lock = std::scoped_lock(state.mutex);
    requestCount = state.setRequests;
  }
  return check(!partial.success && partial.liveApplied &&
                   !partial.persistenceApplied,
               "persistence failure must report partial live success") &&
         check(partial.error.find("applied live") != std::string::npos,
               "partial rate success must be explicit") &&
         check(requestCount == 2, "daemon rate request count differs");
}

static bool testOfflineChange(
    const std::filesystem::path &configPath,
    const std::filesystem::path &missingSocket) {
  const auto policy = pipetune::defaultSampleRatePolicy();
  const auto changed = pipetune::executeSetSampleRatePolicy(
      {.configPath = configPath, .socketPath = missingSocket}, policy);
  return check(changed.success && !changed.liveApplied &&
                   changed.persistenceApplied,
               "offline rate set must persist for the next daemon start") &&
         check(!changed.notice.empty(),
               "offline rate set must explain deferred live application") &&
         configHasPolicy(configPath, policy);
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-rate-command-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto socketPath = directory / "control.sock";
  auto state = ServerState{
      .mutex = {},
      .rejectChanges = false,
      .policy = pipetune::defaultSampleRatePolicy(),
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
