#include "command_line.h"

#include "pipetune/control_protocol.h"

#include <array>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

int main() {
  const auto automaticArguments =
      std::array<std::string_view, 3>{"rate", "set", "automatic"};
  const auto legacyArguments =
      std::array<std::string_view, 4>{"rate", "set", "max", "suggest"};
  const auto automatic = pipetune::parseCommandLine(automaticArguments);
  const auto legacy = pipetune::parseCommandLine(legacyArguments);

  auto status = pipetune::ControlRuntimeStatus{};
  status.configuredRatePolicy = pipetune::defaultSampleRatePolicy();
  status.dspSampleRate = 48000;
  const auto response = pipetune::makeControlSuccessResponse(status, {});

  return check(automatic.error.empty(),
               "automatic graph-rate selection must be accepted") &&
         check(pipetune::sampleRateModeName(
                   automatic.options.ratePolicy.mode) == "automatic",
               "automatic graph-rate selection mode differs") &&
         check(!legacy.error.empty(),
               "legacy maximum-device selection must be rejected") &&
         check(response.find("\"graphSampleRate\"") !=
                   std::string::npos,
               "status must publish the negotiated graph rate") &&
         check(response.find("selectedOutputSampleRate") ==
                   std::string::npos &&
                   response.find("activeOutputSampleRate") ==
                       std::string::npos &&
                   response.find("rateFallback") == std::string::npos,
               "status must not publish physical-output rate state")
             ? 0
             : 1;
}
