/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_CONFIGURATION_RESET_CLIENT_H
#define PIPETUNE_GTK_CONFIGURATION_RESET_CLIENT_H

#include <filesystem>
#include <string>

namespace pipetune_gtk {

/** Opaque asynchronous configuration-reset client state. */
struct ConfigurationResetClient;

/**
 * Reports one completed CLI configuration-reset request.
 */
struct ConfigurationResetClientResult {
  /** True when the CLI completed successfully. */
  bool success;
  /** Trimmed CLI standard output. */
  std::string standardOutput;
  /** Spawn, communication, or CLI diagnostic, or empty on success. */
  std::string error;
};

/**
 * Receives one asynchronous configuration-reset result.
 *
 * @param result Completed CLI result.
 * @param userData Opaque callback argument.
 */
using ConfigurationResetClientCallback = void (*)(
    const ConfigurationResetClientResult &result, void *userData);

/**
 * Creates a client that launches the installed PipeTune CLI.
 *
 * @param pipeTuneExecutable Absolute PipeTune CLI path.
 * @return Client that must be released with
 * destroyConfigurationResetClient(), or null for an invalid path.
 */
ConfigurationResetClient *createConfigurationResetClient(
    const std::filesystem::path &pipeTuneExecutable);

/**
 * Cancels pending communication and releases a reset client.
 *
 * Completion callbacks are suppressed after this function returns. The
 * already-started CLI process is not forcibly terminated.
 *
 * @param client Client to release, or null.
 */
void destroyConfigurationResetClient(ConfigurationResetClient *client);

/**
 * Asynchronously runs `pipetune config reset --yes`.
 *
 * At most one reset can be pending per client. The callback is always
 * dispatched from the GLib main context after this function returns.
 *
 * @param client Client used for the request.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 * @return True when completion was scheduled, otherwise false.
 */
bool resetConfigurationAsync(ConfigurationResetClient *client,
                             ConfigurationResetClientCallback callback,
                             void *userData);

} // namespace pipetune_gtk

#endif
