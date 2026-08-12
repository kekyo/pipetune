#ifndef PIPETUNE_GTK_USER_SETUP_CLIENT_H
#define PIPETUNE_GTK_USER_SETUP_CLIENT_H

#include <filesystem>
#include <string>

namespace pipetune_gtk {

/** Opaque asynchronous per-user setup client state. */
struct UserSetupClient;

/**
 * Reports one completed CLI per-user setup request.
 */
struct UserSetupClientResult {
  /** True when the CLI completed successfully. */
  bool success;
  /** Trimmed CLI standard output. */
  std::string standardOutput;
  /** Spawn, communication, or CLI diagnostic, or empty on success. */
  std::string error;
};

/**
 * Receives one asynchronous per-user setup result.
 *
 * @param result Completed CLI result.
 * @param userData Opaque callback argument.
 */
using UserSetupClientCallback = void (*)(
    const UserSetupClientResult &result, void *userData);

/**
 * Creates a client that launches the installed PipeTune CLI.
 *
 * @param pipeTuneExecutable Absolute PipeTune CLI path.
 * @return Client that must be released with destroyUserSetupClient(), or null
 * for an invalid path.
 */
UserSetupClient *createUserSetupClient(
    const std::filesystem::path &pipeTuneExecutable);

/**
 * Cancels pending communication and releases a setup client.
 *
 * Completion callbacks are suppressed after this function returns. The
 * already-started CLI process is not forcibly terminated.
 *
 * @param client Client to release, or null.
 */
void destroyUserSetupClient(UserSetupClient *client);

/**
 * Asynchronously runs `pipetune setup --no-launch-gtk`.
 *
 * At most one setup can be pending per client. The callback is always
 * dispatched from the GLib main context after this function returns.
 *
 * @param client Client used for the request.
 * @param callback Non-null completion callback.
 * @param userData Opaque callback argument.
 * @return True when completion was scheduled, otherwise false.
 */
bool setupUserIfNeededAsync(UserSetupClient *client,
                            UserSetupClientCallback callback,
                            void *userData);

} // namespace pipetune_gtk

#endif
