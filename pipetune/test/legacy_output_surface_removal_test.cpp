#include "pipetune/control_protocol.h"
#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool controlProtocolDoesNotOwnOutputs() {
  const auto setOutput = pipetune::parseControlRequest(
      R"json({"command":"set-output","target":"alsa_output.test"})json");
  const auto clearOutput = pipetune::parseControlRequest(
      R"json({"command":"clear-output"})json");

  auto status = pipetune::ControlRuntimeStatus{};
  status.configuredRatePolicy = pipetune::defaultSampleRatePolicy();
  status.dspSampleRate = 48000;
  const auto response = pipetune::makeControlSuccessResponse(status, {});

  return check(!setOutput.error.empty(),
               "set-output control requests must be rejected") &&
         check(!clearOutput.error.empty(),
               "clear-output control requests must be rejected") &&
         check(!response.empty(), "status response must be serializable") &&
         check(response.find("preferredTarget") == std::string::npos &&
                   response.find("selectedTarget") == std::string::npos &&
                   response.find("outputSelectionReason") ==
                       std::string::npos &&
                   response.find("availableOutputs") == std::string::npos &&
                   response.find("defaultSinkActive") == std::string::npos &&
                   response.find("sampleRateCapabilitiesKnown") ==
                       std::string::npos,
               "status must not expose physical-output ownership");
}

static bool startupConfigDoesNotAcceptOutputPreference() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-output-removal-" + std::to_string(getpid()));
  auto filesystemError = std::error_code{};
  std::filesystem::create_directories(directory, filesystemError);
  if (!check(!filesystemError, "temporary directory creation failed")) {
    return false;
  }

  const auto configPath = directory / "environment";
  auto stream = std::ofstream(configPath, std::ios::binary);
  stream << "PIPETUNE_TARGET=alsa_output.test\n";
  stream.close();
  const auto loaded = pipetune::loadStartupConfig(configPath);
  std::filesystem::remove_all(directory, filesystemError);

  return check(!loaded.error.empty(),
               "PIPETUNE_TARGET must not be accepted as configuration");
}

static bool serviceDoesNotRestorePhysicalOutput(
    const std::filesystem::path &servicePath) {
  auto stream = std::ifstream(servicePath, std::ios::binary);
  const auto contents = std::string(std::istreambuf_iterator<char>{stream},
                                    std::istreambuf_iterator<char>{});
  return check(stream.good() || stream.eof(), "service file cannot be read") &&
         check(contents.find("restore-default") == std::string::npos &&
                   contents.find("ExecStopPost") == std::string::npos,
               "service must not restore a PipeTune-owned output route");
}

int main(int argc, char **argv) {
  if (!check(argc == 2, "service path argument is required")) {
    return 1;
  }
  return controlProtocolDoesNotOwnOutputs() &&
                 startupConfigDoesNotAcceptOutputPreference() &&
                 serviceDoesNotRestorePhysicalOutput(argv[1])
             ? 0
             : 1;
}
