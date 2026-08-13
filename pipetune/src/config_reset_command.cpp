/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "config_reset_command.h"

#include "pipetune/startup_config.h"

#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune {

static bool isAsciiWhitespace(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' ||
         character == '\r' || character == '\f' || character == '\v';
}

static char lowercaseAscii(char character) noexcept {
  if (character >= 'A' && character <= 'Z') {
    return static_cast<char>(character - 'A' + 'a');
  }
  return character;
}

bool configurationResetIsConfirmed(std::string_view response) noexcept {
  while (!response.empty() && isAsciiWhitespace(response.front())) {
    response.remove_prefix(1);
  }
  while (!response.empty() && isAsciiWhitespace(response.back())) {
    response.remove_suffix(1);
  }

  auto normalized = std::string{};
  normalized.reserve(response.size());
  for (const auto character : response) {
    normalized.push_back(lowercaseAscii(character));
  }
  return normalized == "y" || normalized == "yes";
}

ConfigurationResetResult
executeConfigurationReset(const ConfigurationResetRequest &request) {
  if (request.processRunner == nullptr) {
    return {.success = false,
            .configurationReset = false,
            .error = "configuration reset process runner is unavailable"};
  }
  if (request.systemctlExecutable.empty()) {
    return {.success = false,
            .configurationReset = false,
            .error = "systemctl executable path is empty"};
  }

  const auto persistenceError = resetStartupConfig(request.configPath);
  if (!persistenceError.empty()) {
    return {.success = false,
            .configurationReset = false,
            .error = persistenceError};
  }

  const auto arguments = std::array<std::string, 3>{
      "--user", "try-restart", "pipetune.service"};
  const auto process =
      request.processRunner(request.systemctlExecutable, arguments,
                            ProcessWaitMode::wait,
                            request.processUserData);
  if (!process.started || !process.error.empty()) {
    auto error =
        std::string{"configuration was reset, but the PipeTune service "
                    "could not be restarted"};
    if (!process.error.empty()) {
      error += ": " + process.error;
    }
    return {.success = false,
            .configurationReset = true,
            .error = std::move(error)};
  }
  if (process.exitCode != 0) {
    return {
        .success = false,
        .configurationReset = true,
        .error = "configuration was reset, but systemctl try-restart exited "
                 "with status " +
                 std::to_string(process.exitCode)};
  }
  return {.success = true, .configurationReset = true, .error = {}};
}

} // namespace pipetune
