/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_CONFIG_RESET_COMMAND_H
#define PIPETUNE_CONFIG_RESET_COMMAND_H

#include "process_runner.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace pipetune {

/**
 * Runs the process used to apply a configuration reset to the live service.
 */
using ConfigurationResetProcessRunner = ProcessResult (*)(
    const std::filesystem::path &executable,
    std::span<const std::string> arguments, ProcessWaitMode mode,
    void *userData);

/**
 * Describes a complete configuration reset request.
 */
struct ConfigurationResetRequest {
  /** Canonical per-user startup configuration path. */
  std::filesystem::path configPath;
  /** Absolute systemctl executable path. */
  std::filesystem::path systemctlExecutable;
  /** Process runner used to restart an active service. */
  ConfigurationResetProcessRunner processRunner;
  /** Opaque value forwarded to processRunner. */
  void *processUserData;
};

/**
 * Reports configuration persistence and live-application status.
 */
struct ConfigurationResetResult {
  /** True when persistence and service application both succeeded. */
  bool success;
  /** True when the default configuration was persisted. */
  bool configurationReset;
  /** Failure diagnostic, or empty on success. */
  std::string error;
};

/**
 * Tests whether a confirmation response authorizes a reset.
 *
 * Leading and trailing ASCII whitespace is ignored. The accepted values are
 * `y` and `yes`, matched case-insensitively.
 *
 * @param response Confirmation text.
 * @return True only when the response authorizes the reset.
 */
bool configurationResetIsConfirmed(std::string_view response) noexcept;

/**
 * Replaces the startup configuration with defaults and applies it live.
 *
 * The replacement is persisted before `systemctl --user try-restart` is run.
 * A restart failure leaves the replacement in place and is reported as a
 * partial failure.
 *
 * @param request Configuration and process dependencies.
 * @return Persistence and service-application result.
 */
ConfigurationResetResult
executeConfigurationReset(const ConfigurationResetRequest &request);

} // namespace pipetune

#endif
