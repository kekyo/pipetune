#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"

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

static bool testOrderlySignalShutdown(pipetune::DspPipeline &pipeline,
                                      std::string_view processId) {
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
        pipeline,
        {.sinkName = "pipetune_signal_test_" + std::string(processId),
         .sinkDescription = "PipeTune signal integration test",
         .targetObject = "",
         .sampleRate = 48000,
         .channelCount = 2,
         .maxFrames = 8192,
         .ringCapacityFrames = 16384,
         .readyCallback = reportReadyToParent,
         .readyUserData = &descriptors[1]},
        pipetune::PipeWireRunMode::untilInterrupted);
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
  return check(waitResult == child && WIFEXITED(childStatus) &&
                   WEXITSTATUS(childStatus) == 0,
               "SIGTERM must stop the PipeWire pipeline orderly");
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

  auto loaded = pipetune::loadDspPipeline(
      presetPath, {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 8192});
  if (!check(loaded.pipeline != nullptr, loaded.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  if (!testOrderlySignalShutdown(*loaded.pipeline, processId)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto readyNotifications = 0;
  const auto result = pipetune::runPipeWirePipeline(
      *loaded.pipeline,
      {.sinkName = "pipetune_test_" + processId,
       .sinkDescription = "PipeTune integration test",
       .targetObject = "",
       .sampleRate = 48000,
       .channelCount = 2,
       .maxFrames = 8192,
       .ringCapacityFrames = 16384,
       .readyCallback = countReadyNotification,
       .readyUserData = &readyNotifications},
      pipetune::PipeWireRunMode::untilReady);

  std::filesystem::remove_all(directory);
  return check(result.success, result.error) &&
                 check(readyNotifications == 1,
                       "PipeWire readiness must be reported exactly once")
             ? 0
             : 1;
}
