#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <poll.h>
#include <span>
#include <string>
#include <string_view>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

struct CommandResult {
  int exitCode;
  std::string output;
};

struct StreamingAudioSource {
  pid_t child;
  int input;
};

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

static std::optional<std::filesystem::path> findExecutable(
    std::string_view name) {
  const auto *pathValue = std::getenv("PATH");
  if (pathValue == nullptr) {
    return std::nullopt;
  }
  auto remaining = std::string_view(pathValue);
  while (true) {
    const auto separator = remaining.find(':');
    const auto directory = remaining.substr(0, separator);
    const auto candidate =
        std::filesystem::path(directory.empty() ? "." : directory) / name;
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
    if (separator == std::string_view::npos) {
      return std::nullopt;
    }
    remaining.remove_prefix(separator + 1);
  }
}

static std::vector<char *> argumentPointers(
    const std::filesystem::path &executable,
    std::span<std::string> arguments,
    std::string &executableString) {
  executableString = executable.string();
  auto pointers = std::vector<char *>{};
  pointers.reserve(arguments.size() + 2);
  pointers.push_back(executableString.data());
  for (auto &argument : arguments) {
    pointers.push_back(argument.data());
  }
  pointers.push_back(nullptr);
  return pointers;
}

static pid_t spawnProcess(const std::filesystem::path &executable,
                          std::vector<std::string> arguments) {
  const auto child = fork();
  if (child != 0) {
    return child;
  }
  auto executableString = std::string{};
  auto pointers =
      argumentPointers(executable, arguments, executableString);
  execv(executableString.c_str(), pointers.data());
  _exit(127);
}

static int waitForProcess(pid_t child) {
  auto status = int{0};
  auto waited = pid_t{-1};
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child) {
    return -1;
  }
  if (WIFEXITED(status)) {
    return WEXITSTATUS(status);
  }
  return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : -1;
}

static void terminateProcess(pid_t &child, int signalNumber) {
  if (child <= 0) {
    return;
  }
  if (kill(child, signalNumber) != 0 && errno != ESRCH) {
    std::cerr << "cannot terminate child process " << child << '\n';
  }
  static_cast<void>(waitForProcess(child));
  child = -1;
}

static CommandResult runCommand(
    const std::filesystem::path &executable,
    std::vector<std::string> arguments) {
  auto descriptors = std::array<int, 2>{-1, -1};
  if (pipe(descriptors.data()) != 0) {
    return {.exitCode = -1, .output = {}};
  }
  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return {.exitCode = -1, .output = {}};
  }
  if (child == 0) {
    close(descriptors[0]);
    dup2(descriptors[1], STDOUT_FILENO);
    dup2(descriptors[1], STDERR_FILENO);
    close(descriptors[1]);
    auto executableString = std::string{};
    auto pointers =
        argumentPointers(executable, arguments, executableString);
    execv(executableString.c_str(), pointers.data());
    _exit(127);
  }

  close(descriptors[1]);
  auto output = std::string{};
  auto buffer = std::array<char, 4096>{};
  while (true) {
    auto count = ssize_t{-1};
    do {
      count = read(descriptors[0], buffer.data(), buffer.size());
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      break;
    }
    output.append(buffer.data(), static_cast<std::size_t>(count));
  }
  close(descriptors[0]);
  return {.exitCode = waitForProcess(child), .output = std::move(output)};
}

static void reportReady(void *userData) {
  const auto descriptor = *static_cast<int *>(userData);
  constexpr auto marker = char{'R'};
  auto result = ssize_t{-1};
  do {
    result = write(descriptor, &marker, 1);
  } while (result < 0 && errno == EINTR);
}

static bool waitForPipelineReadiness(int descriptor) {
  auto event = pollfd{.fd = descriptor, .events = POLLIN, .revents = 0};
  auto result = int{-1};
  do {
    result = poll(&event, 1, 10000);
  } while (result < 0 && errno == EINTR);
  if (result != 1 || (event.revents & POLLIN) == 0) {
    return false;
  }
  auto marker = char{0};
  auto count = ssize_t{-1};
  do {
    count = read(descriptor, &marker, 1);
  } while (count < 0 && errno == EINTR);
  return count == 1 && marker == 'R';
}

static bool writeFloatWave(const std::filesystem::path &path,
                           std::uint32_t frameCount, float amplitude) {
  constexpr auto channelCount = std::uint16_t{2};
  constexpr auto sampleRate = std::uint32_t{48000};
  constexpr auto bytesPerSample = std::uint16_t{4};
  const auto dataSize =
      frameCount * channelCount * bytesPerSample;
  auto stream = std::ofstream(path, std::ios::binary);
  if (!stream) {
    return false;
  }

  const auto riffSize = std::uint32_t{36} + dataSize;
  const auto formatSize = std::uint32_t{16};
  const auto floatFormat = std::uint16_t{3};
  const auto byteRate =
      sampleRate * channelCount * bytesPerSample;
  const auto blockAlign =
      static_cast<std::uint16_t>(channelCount * bytesPerSample);
  const auto bitsPerSample = std::uint16_t{32};
  stream.write("RIFF", 4);
  stream.write(reinterpret_cast<const char *>(&riffSize),
               sizeof(riffSize));
  stream.write("WAVEfmt ", 8);
  stream.write(reinterpret_cast<const char *>(&formatSize),
               sizeof(formatSize));
  stream.write(reinterpret_cast<const char *>(&floatFormat),
               sizeof(floatFormat));
  stream.write(reinterpret_cast<const char *>(&channelCount),
               sizeof(channelCount));
  stream.write(reinterpret_cast<const char *>(&sampleRate),
               sizeof(sampleRate));
  stream.write(reinterpret_cast<const char *>(&byteRate),
               sizeof(byteRate));
  stream.write(reinterpret_cast<const char *>(&blockAlign),
               sizeof(blockAlign));
  stream.write(reinterpret_cast<const char *>(&bitsPerSample),
               sizeof(bitsPerSample));
  stream.write("data", 4);
  stream.write(reinterpret_cast<const char *>(&dataSize),
               sizeof(dataSize));
  for (auto frame = std::uint32_t{0}; frame < frameCount; ++frame) {
    for (auto channel = std::uint16_t{0}; channel < channelCount;
         ++channel) {
      stream.write(reinterpret_cast<const char *>(&amplitude),
                   sizeof(amplitude));
    }
  }
  return stream.good();
}

static std::optional<std::uint32_t> parseNodeId(
    std::string_view output) {
  const auto marker = output.find("id:");
  if (marker == std::string_view::npos) {
    return std::nullopt;
  }
  const auto start = output.find_first_of("0123456789", marker + 3);
  if (start == std::string_view::npos) {
    return std::nullopt;
  }
  const auto end = output.find_first_not_of("0123456789", start);
  const auto value = std::string(output.substr(start, end - start));
  char *parsedEnd = nullptr;
  const auto parsed = std::strtoul(value.c_str(), &parsedEnd, 10);
  if (parsedEnd == value.c_str() || *parsedEnd != '\0') {
    return std::nullopt;
  }
  return static_cast<std::uint32_t>(parsed);
}

static std::optional<float> parseFirstChannelVolume(
    std::string_view output) {
  const auto property = output.find(":channelVolumes");
  if (property == std::string_view::npos) {
    return std::nullopt;
  }
  const auto marker = output.find("Float ", property);
  if (marker == std::string_view::npos) {
    return std::nullopt;
  }
  const auto valueStart = marker + std::string_view("Float ").size();
  const auto valueEnd = output.find_first_of("\r\n", valueStart);
  const auto value =
      std::string(output.substr(valueStart, valueEnd - valueStart));
  char *parsedEnd = nullptr;
  const auto parsed = std::strtof(value.c_str(), &parsedEnd);
  if (parsedEnd == value.c_str() || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

static std::optional<bool> parseMute(std::string_view output) {
  const auto property = output.find(":mute ");
  if (property == std::string_view::npos) {
    return std::nullopt;
  }
  const auto trueValue = output.find("Bool true", property);
  const auto falseValue = output.find("Bool false", property);
  if (trueValue == std::string_view::npos &&
      falseValue == std::string_view::npos) {
    return std::nullopt;
  }
  return trueValue != std::string_view::npos &&
         (falseValue == std::string_view::npos ||
          trueValue < falseValue);
}

static std::optional<float> parseDisplayedVolume(
    std::string_view output) {
  const auto marker = output.find("Volume:");
  if (marker == std::string_view::npos) {
    return std::nullopt;
  }
  const auto valueStart =
      output.find_first_of("0123456789", marker);
  if (valueStart == std::string_view::npos) {
    return std::nullopt;
  }
  const auto value =
      std::string(output.substr(valueStart));
  char *parsedEnd = nullptr;
  const auto parsed = std::strtof(value.c_str(), &parsedEnd);
  if (parsedEnd == value.c_str() || !std::isfinite(parsed)) {
    return std::nullopt;
  }
  return parsed;
}

static std::vector<float> longRunAmplitudes(
    const std::filesystem::path &path,
    std::size_t minimumSampleCount) {
  auto stream = std::ifstream(path, std::ios::binary);
  if (!stream) {
    return {};
  }
  auto amplitudes = std::vector<float>{};
  auto currentRun = std::vector<float>{};
  const auto finishRun = [&] {
    if (currentRun.size() >= minimumSampleCount) {
      const auto middle =
          currentRun.begin() + currentRun.size() / 2;
      std::nth_element(currentRun.begin(), middle,
                       currentRun.end());
      amplitudes.push_back(*middle);
    }
    currentRun.clear();
  };
  auto sample = float{0.0F};
  while (stream.read(reinterpret_cast<char *>(&sample), sizeof(sample))) {
    const auto amplitude = std::abs(sample);
    if (std::isfinite(amplitude) && amplitude > 0.000001F) {
      currentRun.push_back(amplitude);
    } else {
      finishRun();
    }
  }
  finishRun();
  return amplitudes;
}

static bool near(float actual, float expected, float tolerance) {
  return std::abs(actual - expected) <= tolerance;
}

static bool runAudioSource(
    const std::filesystem::path &pwCat,
    std::string_view target,
    const std::filesystem::path &wavePath,
    std::string_view sourceName) {
  const auto properties =
      "{ node.name = \"" + std::string(sourceName) +
      "\" media.role = \"Test\" state.restore-props = false }";
  const auto child = spawnProcess(
      pwCat,
      {"--playback", "--target=" + std::string(target),
       "--properties=" + properties, "--volume=1.0",
       wavePath.string()});
  return child > 0 && waitForProcess(child) == 0;
}

static StreamingAudioSource startStreamingAudioSource(
    const std::filesystem::path &pwCat,
    std::string_view target, std::string_view sourceName) {
  auto descriptors = std::array<int, 2>{-1, -1};
  if (pipe(descriptors.data()) != 0) {
    return {.child = -1, .input = -1};
  }
  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return {.child = -1, .input = -1};
  }
  if (child == 0) {
    close(descriptors[1]);
    dup2(descriptors[0], STDIN_FILENO);
    close(descriptors[0]);
    const auto properties =
        "{ node.name = \"" + std::string(sourceName) +
        "\" media.role = \"Test\" state.restore-props = false }";
    auto executableString = std::string{};
    auto arguments = std::vector<std::string>{
        "--playback", "--target=" + std::string(target),
        "--properties=" + properties, "--volume=1.0",
        "--format=f32", "--rate=48000", "--channels=2", "-"};
    auto pointers =
        argumentPointers(pwCat, arguments, executableString);
    execv(executableString.c_str(), pointers.data());
    _exit(127);
  }
  close(descriptors[0]);
  return {.child = child, .input = descriptors[1]};
}

static bool writeStreamingFrames(
    int descriptor, std::uint32_t frameCount, float amplitude) {
  auto samples = std::array<float, 4096>{};
  samples.fill(amplitude);
  auto remaining =
      static_cast<std::size_t>(frameCount) * 2;
  while (remaining != 0) {
    const auto count = std::min(remaining, samples.size());
    const auto *data = reinterpret_cast<const char *>(samples.data());
    auto bytesRemaining = count * sizeof(float);
    while (bytesRemaining != 0) {
      auto written = ssize_t{-1};
      do {
        written = write(descriptor, data, bytesRemaining);
      } while (written < 0 && errno == EINTR);
      if (written <= 0) {
        return false;
      }
      data += written;
      bytesRemaining -= static_cast<std::size_t>(written);
    }
    remaining -= count;
  }
  return true;
}

static std::uintmax_t fileSize(
    const std::filesystem::path &path) {
  auto error = std::error_code{};
  const auto size = std::filesystem::file_size(path, error);
  return error ? 0 : size;
}

static bool waitForFileSize(
    const std::filesystem::path &path,
    std::uintmax_t minimumSize) {
  const auto descriptor = inotify_init1(IN_CLOEXEC);
  if (descriptor < 0) {
    return false;
  }
  const auto watch = inotify_add_watch(
      descriptor, path.c_str(), IN_MODIFY | IN_CLOSE_WRITE);
  if (watch < 0) {
    close(descriptor);
    return false;
  }

  const auto deadline =
      std::chrono::steady_clock::now() +
      std::chrono::seconds(10);
  auto reached = fileSize(path) >= minimumSize;
  auto events = std::array<char, 4096>{};
  while (!reached) {
    const auto remaining = std::chrono::duration_cast<
        std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    if (remaining.count() <= 0) {
      break;
    }
    auto event = pollfd{
        .fd = descriptor, .events = POLLIN, .revents = 0};
    auto result = int{-1};
    do {
      result = poll(
          &event, 1, static_cast<int>(remaining.count()));
    } while (result < 0 && errno == EINTR);
    if (result != 1 || (event.revents & POLLIN) == 0) {
      break;
    }
    auto count = ssize_t{-1};
    do {
      count = read(descriptor, events.data(), events.size());
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      break;
    }
    reached = fileSize(path) >= minimumSize;
  }
  inotify_rm_watch(descriptor, watch);
  close(descriptor);
  return reached;
}

static bool streamingSourceIsRunning(
    StreamingAudioSource &source) {
  auto status = int{0};
  const auto result = waitpid(source.child, &status, WNOHANG);
  if (result == 0) {
    return true;
  }
  source.child = -1;
  return false;
}

static bool finishStreamingAudioSource(
    StreamingAudioSource &source) {
  if (source.input >= 0) {
    close(source.input);
    source.input = -1;
  }
  if (source.child <= 0) {
    return false;
  }
  const auto result = waitForProcess(source.child) == 0;
  source.child = -1;
  return result;
}

static void terminateStreamingAudioSource(
    StreamingAudioSource &source) {
  if (source.input >= 0) {
    close(source.input);
    source.input = -1;
  }
  terminateProcess(source.child, SIGTERM);
}

static pid_t startTestSink(
    const std::filesystem::path &pwCat, std::string_view name,
    std::string_view description, std::string_view volume,
    const std::filesystem::path &capturePath) {
  const auto properties =
      "{ node.name = \"" + std::string(name) +
      "\" media.class = \"Audio/Sink\" node.virtual = false "
      "node.description = \"" + std::string(description) +
      "\" state.restore-props = false }";
  return spawnProcess(
      pwCat,
      {"--record", "--target=0", "--properties=" + properties,
       "--format=f32", "--rate=48000", "--channels=2",
       "--volume=" + std::string(volume), capturePath.string()});
}

static pid_t startPipeline(
    std::string_view sinkName, std::string_view target,
    const std::filesystem::path &socketPath, int readyDescriptor) {
  const auto child = fork();
  if (child != 0) {
    return child;
  }
  auto created = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 8192});
  if (created.pipeline == nullptr) {
    std::cerr << created.error << '\n';
    _exit(1);
  }
  const auto result = pipetune::runPipeWirePipeline(
      std::move(created.pipeline),
      {.sinkName = std::string(sinkName),
       .sinkDescription = "PipeTune volume-routing integration test",
       .targetObject = std::string(target),
       .initialPresetPath = {},
       .initialConfigurationError = {},
       .controlSocketPath = socketPath,
       .dspSampleRate = 48000,
       .outputSampleRate = 48000,
       .ratePolicy =
           {.mode = pipetune::SampleRateMode::fixed,
            .fixedRate = 48000,
            .enforcement =
                pipetune::SampleRateEnforcement::suggest},
       .channelCount = 2,
       .maxFrames = 8192,
       .ringCapacityFrames = 16384,
       .manageDefaultSink = false,
       .readyCallback = reportReady,
       .readyUserData = &readyDescriptor},
      pipetune::PipeWireRunMode::untilInterrupted);
  if (!result.success) {
    std::cerr << result.error << '\n';
  }
  close(readyDescriptor);
  _exit(result.success ? 0 : 1);
}

int main() {
  std::signal(SIGPIPE, SIG_IGN);
  const auto pwCat = findExecutable("pw-cat");
  const auto pwCli = findExecutable("pw-cli");
  const auto wpctl = findExecutable("wpctl");
  if (!pipeWireSessionIsAvailable() || !pwCat.has_value() ||
      !pwCli.has_value() || !wpctl.has_value()) {
    std::cout << "PipeWire test tools are unavailable; skipping volume-routing integration test\n";
    return 77;
  }

  const auto processId =
      std::to_string(static_cast<long long>(getpid()));
  const auto directory = std::filesystem::temp_directory_path() /
                         ("pipetune-volume-routing-" + processId);
  std::filesystem::create_directories(directory);
  const auto longWave = directory / "long.wav";
  const auto shortWave = directory / "short.wav";
  const auto captureA = directory / "capture-a.raw";
  const auto captureB = directory / "capture-b.raw";
  const auto socketPath = directory / "control.sock";
  const auto sinkAName = "pipetune_test_output_a_" + processId;
  const auto sinkBName = "pipetune_test_output_b_" + processId;
  const auto virtualName = "pipetune_test_virtual_" + processId;

  if (!check(writeFloatWave(longWave, 96000, 0.5F) &&
                 writeFloatWave(shortWave, 4800, 0.5F),
             "cannot create volume-routing test audio")) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto sinkA = startTestSink(*pwCat, sinkAName, "PipeTune test output A",
                             "0.25", captureA);
  auto sinkB = startTestSink(*pwCat, sinkBName, "PipeTune test output B",
                             "0.216", captureB);
  auto descriptors = std::array<int, 2>{-1, -1};
  if (sinkA <= 0 || sinkB <= 0 || pipe(descriptors.data()) != 0) {
    terminateProcess(sinkA, SIGINT);
    terminateProcess(sinkB, SIGINT);
    std::filesystem::remove_all(directory);
    return check(false, "cannot start isolated PipeWire test outputs") ? 0 : 1;
  }

  auto pipeline =
      startPipeline(virtualName, sinkAName, socketPath, descriptors[1]);
  close(descriptors[1]);
  auto passed = pipeline > 0 &&
                waitForPipelineReadiness(descriptors[0]);
  close(descriptors[0]);
  if (!check(passed, "volume-routing pipeline did not become ready")) {
    terminateProcess(pipeline, SIGTERM);
    terminateProcess(sinkA, SIGINT);
    terminateProcess(sinkB, SIGINT);
    std::filesystem::remove_all(directory);
    return 1;
  }

  const auto virtualInfo =
      runCommand(*pwCli, {"info", virtualName});
  const auto sinkAInfo = runCommand(*pwCli, {"info", sinkAName});
  const auto sinkBInfo = runCommand(*pwCli, {"info", sinkBName});
  const auto virtualId = parseNodeId(virtualInfo.output);
  const auto sinkAId = parseNodeId(sinkAInfo.output);
  const auto sinkBId = parseNodeId(sinkBInfo.output);
  passed = check(virtualInfo.exitCode == 0 && virtualId.has_value() &&
                     sinkAInfo.exitCode == 0 && sinkAId.has_value() &&
                     sinkBInfo.exitCode == 0 && sinkBId.has_value(),
                 "cannot resolve isolated PipeWire node ids");

  auto initialPhysicalVolumeB = std::optional<float>{};
  if (passed) {
    const auto virtualInitial = runCommand(
        *wpctl, {"get-volume", std::to_string(*virtualId)});
    const auto physicalInitial = runCommand(
        *wpctl, {"get-volume", std::to_string(*sinkAId)});
    const auto initialPropsB =
        runCommand(*pwCli, {"enum-params", sinkBName, "Props"});
    const auto virtualInitialVolume =
        parseDisplayedVolume(virtualInitial.output);
    const auto physicalInitialVolume =
        parseDisplayedVolume(physicalInitial.output);
    initialPhysicalVolumeB =
        parseFirstChannelVolume(initialPropsB.output);
    passed =
        check(virtualInitial.exitCode == 0 &&
                  physicalInitial.exitCode == 0 &&
                  virtualInitialVolume.has_value() &&
                  physicalInitialVolume.has_value() &&
                  near(*virtualInitialVolume, *physicalInitialVolume,
                       0.01F),
              "startup must adopt the selected physical output volume") &&
        check(initialPropsB.exitCode == 0 &&
                  initialPhysicalVolumeB.has_value(),
              "cannot read output B's saved volume before switching");
  }

  if (passed) {
    const auto setVolume = runCommand(
        *wpctl, {"set-volume", std::to_string(*virtualId), "0.5"});
    passed = check(setVolume.exitCode == 0,
                   "cannot set virtual sink volume") &&
             check(runAudioSource(
                       *pwCat, virtualName, longWave,
                       "pipetune_test_source_a_" + processId),
                   "cannot play bypass test audio through output A");
  }

  auto physicalVolumeA = std::optional<float>{};
  if (passed) {
    const auto propsA =
        runCommand(*pwCli, {"enum-params", sinkAName, "Props"});
    physicalVolumeA = parseFirstChannelVolume(propsA.output);
    passed = check(propsA.exitCode == 0 &&
                       physicalVolumeA.has_value() &&
                       !near(*physicalVolumeA, 0.25F, 0.01F),
                   "virtual sink volume must update physical output A");
    passed =
        passed &&
        check(runAudioSource(
                  *pwCat, sinkAName, longWave,
                  "pipetune_test_direct_source_a_" + processId),
              "cannot play direct reference audio through output A");
  }

  if (passed) {
    const auto setMute = runCommand(
        *wpctl, {"set-mute", std::to_string(*virtualId), "1"});
    passed = check(setMute.exitCode == 0,
                   "cannot mute virtual sink") &&
             check(runAudioSource(
                       *pwCat, virtualName, shortWave,
                       "pipetune_test_muted_source_" + processId),
                   "cannot play muted test audio");
    const auto propsA =
        runCommand(*pwCli, {"enum-params", sinkAName, "Props"});
    const auto physicalMute = parseMute(propsA.output);
    passed = passed &&
             check(propsA.exitCode == 0 &&
                       physicalMute.has_value() && *physicalMute,
                   "virtual sink mute must update physical output A");
  }

  if (passed) {
    const auto unmuteA = runCommand(
        *wpctl, {"set-mute", std::to_string(*sinkAId), "0"});
    const auto setPhysicalA = runCommand(
        *wpctl, {"set-volume", std::to_string(*sinkAId), "0.4"});
    passed = check(unmuteA.exitCode == 0 &&
                       setPhysicalA.exitCode == 0,
                   "cannot change physical output A externally") &&
             check(runAudioSource(
                       *pwCat, virtualName, shortWave,
                       "pipetune_test_external_source_" + processId),
                   "cannot drive external-volume synchronization");
    const auto virtualAfterPhysical = runCommand(
        *wpctl, {"get-volume", std::to_string(*virtualId)});
    const auto physicalAfterPhysical = runCommand(
        *wpctl, {"get-volume", std::to_string(*sinkAId)});
    const auto virtualVolume =
        parseDisplayedVolume(virtualAfterPhysical.output);
    const auto physicalVolume =
        parseDisplayedVolume(physicalAfterPhysical.output);
    passed =
        passed &&
        check(virtualAfterPhysical.exitCode == 0 &&
                  physicalAfterPhysical.exitCode == 0 &&
                  virtualVolume.has_value() &&
                  physicalVolume.has_value() &&
                  near(*virtualVolume, *physicalVolume, 0.01F) &&
                  virtualAfterPhysical.output.find("[MUTED]") ==
                      std::string::npos,
              "external physical controls must be reflected by the virtual sink");
  }

  auto physicalVolumeB = std::optional<float>{};
  auto displayedVolumeB = std::optional<float>{};
  auto requestedDisplayedVolumeB = float{0.0F};
  if (passed) {
    const auto selection = pipetune::exchangeControlMessage(
        socketPath,
        pipetune::makeSetOutputControlRequest(sinkBName));
    passed =
        check(selection.error.empty() &&
                  pipetune::inspectControlResponse(selection.response).success,
              "cannot switch to PipeWire test output B");
  }

  if (passed) {
    const auto captureStart = fileSize(captureB);
    auto source = startStreamingAudioSource(
        *pwCat, virtualName,
        "pipetune_test_live_source_b_" + processId);
    passed = check(source.child > 0 && source.input >= 0,
                   "cannot start continuous bypass source on output B");
    constexpr auto toneFrames = std::uint32_t{60000};
    constexpr auto silenceFrames = std::uint32_t{4800};
    if (passed) {
      passed =
          check(writeStreamingFrames(
                    source.input, toneFrames, 0.5F) &&
                    writeStreamingFrames(
                        source.input, silenceFrames, 0.0F),
                "cannot write the pre-change bypass segment") &&
          check(waitForFileSize(
                    captureB,
                    captureStart +
                        static_cast<std::uintmax_t>(
                            toneFrames + silenceFrames) *
                            2 * sizeof(float)),
                "output B did not capture the pre-change segment") &&
          check(streamingSourceIsRunning(source),
                "continuous bypass source ended before the live volume change");
    }

    if (passed) {
      const auto virtualAfterSwitch = runCommand(
          *wpctl, {"get-volume", std::to_string(*virtualId)});
      const auto physicalAfterSwitch = runCommand(
          *wpctl, {"get-volume", std::to_string(*sinkBId)});
      const auto virtualVolume =
          parseDisplayedVolume(virtualAfterSwitch.output);
      const auto physicalDisplayed =
          parseDisplayedVolume(physicalAfterSwitch.output);
      displayedVolumeB = physicalDisplayed;
      const auto propsB =
          runCommand(*pwCli, {"enum-params", sinkBName, "Props"});
      physicalVolumeB = parseFirstChannelVolume(propsB.output);
      passed =
          check(virtualAfterSwitch.exitCode == 0 &&
                    physicalAfterSwitch.exitCode == 0 &&
                    virtualVolume.has_value() &&
                    physicalDisplayed.has_value() &&
                    near(*virtualVolume, *physicalDisplayed, 0.01F),
                "output switch must adopt output B's saved volume") &&
          check(propsB.exitCode == 0 &&
                    physicalVolumeB.has_value() &&
                    initialPhysicalVolumeB.has_value() &&
                    near(*physicalVolumeB,
                         *initialPhysicalVolumeB, 0.01F),
                "output switch must preserve output B's saved volume; before=" +
                    std::to_string(
                        initialPhysicalVolumeB.value_or(-1.0F)) +
                    ", after=" +
                    std::to_string(
                        physicalVolumeB.value_or(-1.0F)));
    }

    if (passed) {
      const auto postChangeCaptureStart = fileSize(captureB);
      requestedDisplayedVolumeB =
          *displayedVolumeB < 0.7F ? 0.8F : 0.4F;
      const auto setVolume = runCommand(
          *wpctl,
          {"set-volume", std::to_string(*virtualId),
           std::to_string(requestedDisplayedVolumeB)});
      passed =
          check(setVolume.exitCode == 0,
                "cannot change virtual volume during playback") &&
          check(writeStreamingFrames(
                    source.input, toneFrames, 0.5F) &&
                    writeStreamingFrames(
                        source.input, silenceFrames, 0.0F),
                "cannot write the post-change bypass segment") &&
          check(finishStreamingAudioSource(source),
                "continuous bypass source failed after the live volume change") &&
          check(waitForFileSize(
                    captureB,
                    postChangeCaptureStart +
                        static_cast<std::uintmax_t>(
                            toneFrames + silenceFrames) *
                            2 * sizeof(float)),
                "output B did not capture the post-change segment");
    }
    if (source.child > 0 || source.input >= 0) {
      terminateStreamingAudioSource(source);
    }

    if (passed) {
      const auto virtualAfterChange = runCommand(
          *wpctl, {"get-volume", std::to_string(*virtualId)});
      const auto physicalAfterChange = runCommand(
          *wpctl, {"get-volume", std::to_string(*sinkBId)});
      const auto propsB =
          runCommand(*pwCli, {"enum-params", sinkBName, "Props"});
      const auto virtualVolume =
          parseDisplayedVolume(virtualAfterChange.output);
      const auto physicalDisplayed =
          parseDisplayedVolume(physicalAfterChange.output);
      const auto changedPhysicalVolume =
          parseFirstChannelVolume(propsB.output);
      passed =
          check(virtualAfterChange.exitCode == 0 &&
                    physicalAfterChange.exitCode == 0 &&
                    virtualVolume.has_value() &&
                    physicalDisplayed.has_value() &&
                    near(*virtualVolume, *physicalDisplayed, 0.01F) &&
                    near(*virtualVolume,
                         requestedDisplayedVolumeB, 0.01F),
                "live virtual volume must converge with output B") &&
          check(propsB.exitCode == 0 &&
                    changedPhysicalVolume.has_value() &&
                    physicalVolumeB.has_value() &&
                    !near(*changedPhysicalVolume,
                          *physicalVolumeB, 0.01F),
                "live virtual volume must update output B; before=" +
                    std::to_string(
                        physicalVolumeB.value_or(-1.0F)) +
                    ", after=" +
                    std::to_string(
                        changedPhysicalVolume.value_or(-1.0F)));
    }

    if (passed) {
      passed =
          check(runAudioSource(
                    *pwCat, sinkBName, longWave,
                    "pipetune_test_direct_source_b_" + processId),
                "cannot play direct reference audio through output B");
    }
  }

  terminateProcess(pipeline, SIGTERM);
  terminateProcess(sinkA, SIGINT);
  terminateProcess(sinkB, SIGINT);

  if (passed) {
    const auto amplitudesA =
        longRunAmplitudes(captureA, 100000);
    const auto amplitudesB =
        longRunAmplitudes(captureB, 100000);
    passed =
        check(amplitudesA.size() >= 2 &&
                  near(amplitudesA[0], amplitudesA[1], 0.005F),
              "Bypass output A must match direct physical playback; bypass=" +
                  std::to_string(
                      amplitudesA.empty() ? -1.0F : amplitudesA[0]) +
                  ", direct=" +
                  std::to_string(
                      amplitudesA.size() < 2 ? -1.0F
                                             : amplitudesA[1])) &&
        check(amplitudesB.size() >= 3 &&
                  near(amplitudesB[1], amplitudesB[2], 0.005F),
              "live-changed Bypass output B must match direct physical "
              "playback; bypass=" +
                  std::to_string(
                      amplitudesB.size() < 2 ? -1.0F
                                             : amplitudesB[1]) +
                  ", direct=" +
                  std::to_string(
                      amplitudesB.size() < 3 ? -1.0F
                                             : amplitudesB[2]));
  }

  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
