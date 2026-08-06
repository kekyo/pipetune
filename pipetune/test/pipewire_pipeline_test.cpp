#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"

#include <yyjson.h>

#include <array>
#include <cstdlib>
#include <cerrno>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool pipeWireSessionIsAvailable() {
  const auto *runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
  if (runtimeDirectory == nullptr || runtimeDirectory[0] == '\0') {
    return false;
  }
  const auto *configuredRemote = std::getenv("PIPEWIRE_REMOTE");
  const auto remote =
      configuredRemote == nullptr || configuredRemote[0] == '\0'
          ? std::filesystem::path("pipewire-0")
          : std::filesystem::path(configuredRemote);
  const auto socket = remote.is_absolute()
                          ? remote
                          : std::filesystem::path(runtimeDirectory) / remote;
  return std::filesystem::exists(socket);
}

static void countReadyNotification(void *userData) {
  auto &count = *static_cast<int *>(userData);
  ++count;
}

static void reportReadyToParent(void *userData) {
  const auto descriptor = *static_cast<int *>(userData);
  constexpr auto marker = char{'R'};
  auto result = ssize_t{-1};
  do {
    result = write(descriptor, &marker, 1);
  } while (result < 0 && errno == EINTR);
}

static bool responseHasLivePreset(std::string_view response,
                                  const std::filesystem::path &presetPath,
                                  std::size_t expectedWarningCount) {
  auto *document = yyjson_read(response.data(), response.size(), 0);
  if (document == nullptr) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document);
  auto *preset = yyjson_is_obj(root) ? yyjson_obj_get(root, "preset") : nullptr;
  auto *warnings =
      yyjson_is_obj(root) ? yyjson_obj_get(root, "warnings") : nullptr;
  const auto matches =
      yyjson_is_str(preset) &&
      std::string_view(yyjson_get_str(preset), yyjson_get_len(preset)) ==
      presetPath.string() &&
      yyjson_get_uint(yyjson_obj_get(root, "activePluginCount")) == 1 &&
      yyjson_is_arr(warnings) &&
      yyjson_arr_size(warnings) == expectedWarningCount;
  yyjson_doc_free(document);
  return matches;
}

static bool testOrderlySignalShutdown(
    std::unique_ptr<pipetune::DspPipeline> pipeline,
    std::string_view processId,
    const std::filesystem::path &initialPresetPath,
    const std::filesystem::path &replacementPresetPath,
    const std::filesystem::path &socketPath) {
  auto descriptors = std::array<int, 2>{-1, -1};
  if (!check(pipe(descriptors.data()) == 0,
             "cannot create readiness pipe for signal test")) {
    return false;
  }

  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return check(false, "cannot fork PipeWire signal test");
  }
  if (child == 0) {
    close(descriptors[0]);
    const auto result = pipetune::runPipeWirePipeline(
        std::move(pipeline),
        {.filterName = "pipetune_signal_test_" + std::string(processId),
         .filterDescription = "PipeTune signal integration test",
         .initialPresetPath = initialPresetPath,
         .initialConfigurationError = {},
         .controlSocketPath = socketPath,
         .dspSampleRate = 48000,
         .ratePolicy =
             {.mode = pipetune::SampleRateMode::fixed,
              .fixedRate = 48000,
              .enforcement =
                  pipetune::SampleRateEnforcement::suggest},
         .channelCount = 2,
         .maxFrames = 8192,
         .ringCapacityFrames = 16384,
         .readyCallback = reportReadyToParent,
         .readyUserData = &descriptors[1]},
        pipetune::PipeWireRunMode::untilInterrupted);
    if (!result.success) {
      std::cerr << result.error << '\n';
    }
    close(descriptors[1]);
    _exit(result.success ? 0 : 1);
  }

  close(descriptors[1]);
  auto marker = char{0};
  auto readResult = ssize_t{-1};
  do {
    readResult = read(descriptors[0], &marker, 1);
  } while (readResult < 0 && errno == EINTR);
  close(descriptors[0]);
  if (readResult != 1 || marker != 'R') {
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return check(false, "child pipeline did not report readiness");
  }

  const auto status = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeStatusControlRequest());
  const auto parsedStatus =
      pipetune::parseControlResponse(status.response);
  if (!check(status.error.empty(), status.error) ||
      !check(pipetune::inspectControlResponse(status.response).success,
             "initial status request failed") ||
      !check(parsedStatus.valid, parsedStatus.error) ||
      !check(parsedStatus.status.inputSampleFormat == "F32P" &&
                 parsedStatus.status.inputSampleRate == 48000 &&
                 parsedStatus.status.inputChannelCount == 2,
             "initial status does not report the negotiated input format")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto rateChange = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeSetRateControlRequest(
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 96000,
           .enforcement = pipetune::SampleRateEnforcement::force}));
  const auto parsedRate =
      pipetune::parseControlResponse(rateChange.response);
  if (!check(rateChange.error.empty(), rateChange.error) ||
      !check(parsedRate.valid, parsedRate.error) ||
      !check(parsedRate.success,
             "live rate request failed") ||
      !check(parsedRate.status.configuredRatePolicy ==
                     pipetune::SampleRatePolicy{
                         .mode = pipetune::SampleRateMode::fixed,
                         .fixedRate = 96000,
                         .enforcement =
                             pipetune::SampleRateEnforcement::force} &&
                 parsedRate.status.dspSampleRate == 96000 &&
                 parsedRate.status.inputSampleRate == 96000 &&
                 parsedRate.status.graphSampleRate != 0 &&
                 !parsedRate.status.rateTransitioning,
             "live rate response does not report completed renegotiation")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto backendChange = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeSetDspBackendControlRequest(
          pipetune::DspBackendKind::simd));
  const auto parsedBackend =
      pipetune::parseControlResponse(backendChange.response);
  if (!check(backendChange.error.empty(), backendChange.error) ||
      !check(parsedBackend.valid, parsedBackend.error) ||
      !check(parsedBackend.success,
             "live DSP backend request failed") ||
      !check(parsedBackend.status.configuredDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 parsedBackend.status.effectiveDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 !parsedBackend.status.dspBackendFallback &&
                 parsedBackend.status.availableDspBackends[0].available &&
                 parsedBackend.status.availableDspBackends[1].available,
             "live DSP backend response does not report effective SIMD")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto load = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeLoadPresetControlRequest(replacementPresetPath));
  if (!check(load.error.empty(), load.error) ||
      !check(pipetune::inspectControlResponse(load.response).success,
             "live preset request failed") ||
      !check(responseHasLivePreset(load.response, replacementPresetPath, 1),
             "live preset response does not report the active replacement")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto rejected = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeLoadPresetControlRequest(
                      replacementPresetPath.parent_path() /
                      "missing.effetune_preset"));
  const auto statusAfterFailure = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeStatusControlRequest());
  if (!check(rejected.error.empty(), rejected.error) ||
      !check(!pipetune::inspectControlResponse(rejected.response).success,
             "missing live preset must be rejected") ||
      !check(statusAfterFailure.error.empty(), statusAfterFailure.error) ||
      !check(responseHasLivePreset(statusAfterFailure.response,
                                   replacementPresetPath, 0),
             "failed loading must leave the previous preset active")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  if (kill(child, SIGTERM) != 0) {
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return check(false, "cannot signal child PipeWire pipeline");
  }

  auto childStatus = 0;
  auto waitResult = pid_t{-1};
  do {
    waitResult = waitpid(child, &childStatus, 0);
  } while (waitResult < 0 && errno == EINTR);
  if (!check(waitResult == child && WIFEXITED(childStatus) &&
                 WEXITSTATUS(childStatus) == 0,
             "SIGTERM must stop the PipeWire pipeline orderly")) {
    return false;
  }
  return true;
}

int main() {
  if (!pipeWireSessionIsAvailable()) {
    std::cout << "PipeWire session socket is unavailable; skipping integration test\n";
    return 77;
  }

  const auto processId = std::to_string(static_cast<long long>(getpid()));
  const auto directory =
      std::filesystem::temp_directory_path() / ("pipetune-pipewire-test-" + processId);
  std::filesystem::create_directories(directory);
  const auto presetPath = directory / "empty.effetune_preset";
  {
    auto preset = std::ofstream(presetPath, std::ios::binary);
    preset << R"json({"name":"PipeWire test","pipeline":[],"timestamp":1})json";
  }
  const auto replacementPresetPath =
      directory / "replacement.effetune_preset";
  {
    auto preset = std::ofstream(replacementPresetPath, std::ios::binary);
    preset << R"json({"pipeline":[
      {"name":"Future DSP","enabled":true,"parameters":{}},
      {"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}
    ]})json";
  }
  const auto socketPath = directory / "control.sock";

  auto signalPipeline = pipetune::loadDspPipeline(
      presetPath, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 8192});
  if (!check(signalPipeline.pipeline != nullptr, signalPipeline.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  if (!testOrderlySignalShutdown(
          std::move(signalPipeline.pipeline), processId, presetPath,
          replacementPresetPath, socketPath)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto readyPipeline = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 8192});
  if (!check(readyPipeline.pipeline != nullptr, readyPipeline.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }
  auto readyNotifications = 0;
  const auto result = pipetune::runPipeWirePipeline(
      std::move(readyPipeline.pipeline),
      {.filterName = "pipetune_test_" + processId,
       .filterDescription = "PipeTune integration test",
       .initialPresetPath = presetPath,
       .initialConfigurationError = {},
       .controlSocketPath = {},
       .dspSampleRate = 48000,
       .ratePolicy = pipetune::defaultSampleRatePolicy(),
       .channelCount = 2,
       .maxFrames = 8192,
       .ringCapacityFrames = 16384,
       .readyCallback = countReadyNotification,
       .readyUserData = &readyNotifications},
      pipetune::PipeWireRunMode::untilReady);

  std::filesystem::remove_all(directory);
  return check(result.success, result.error) &&
                 check(readyNotifications == 1,
                       "PipeWire readiness must be reported exactly once") &&
                 check(result.processingErrors == 0,
                       "readiness must not report processing errors")
             ? 0
             : 1;
}
