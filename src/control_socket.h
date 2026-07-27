#ifndef PIPETUNE_CONTROL_SOCKET_H
#define PIPETUNE_CONTROL_SOCKET_H

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace pipetune {

/**
 * Reports a resolved explicit or XDG control-socket path.
 */
struct ControlSocketPathResult {
  /** Resolved socket path, or empty when error is non-empty. */
  std::filesystem::path path;
  /** Resolution diagnostic, or empty on success. */
  std::string error;
};

/**
 * Resolves a configured path or the per-user XDG default.
 *
 * @param configuredPath Explicit path, or empty to use
 * `$XDG_RUNTIME_DIR/pipetune/control.sock`.
 * @return Resolved path or a missing-environment diagnostic.
 */
ControlSocketPathResult
resolveControlSocketPath(const std::filesystem::path &configuredPath);

/**
 * Handles one complete control message outside the audio thread.
 *
 * @param request Message body without framing newline.
 * @param userData Opaque pointer supplied to startControlServer().
 * @return Response body without framing newline.
 */
using ControlMessageHandler =
    std::string (*)(std::string_view request, void *userData);

struct ControlServerStartResult;

/**
 * Owns a same-user Unix-domain control endpoint and its service thread.
 */
class ControlServer final {
public:
  /** Opaque native socket and service-thread state. */
  struct Impl;

  /** Stops request handling and removes the owned socket path. */
  ~ControlServer();
  /** Servers cannot be copied. */
  ControlServer(const ControlServer &) = delete;
  /** Servers cannot be copy-assigned. */
  ControlServer &operator=(const ControlServer &) = delete;

private:
  explicit ControlServer(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;

  friend ControlServerStartResult
  startControlServer(const std::filesystem::path &socketPath,
                     ControlMessageHandler handler, void *userData);
};

/**
 * Reports control-server startup or one fatal diagnostic.
 */
struct ControlServerStartResult {
  /** Running server, or null when error is non-empty. */
  std::unique_ptr<ControlServer> server;
  /** Startup diagnostic, or empty on success. */
  std::string error;
};

/**
 * Creates a user-only local control server.
 *
 * A live socket is never replaced. A stale socket is removed only when it is a
 * socket owned by the effective user and no process accepts connections.
 *
 * @param socketPath Filesystem path for the Unix-domain socket.
 * @param handler Non-null message callback executed on the service thread.
 * @param userData Opaque callback argument.
 * @return Running server or a startup diagnostic.
 */
ControlServerStartResult
startControlServer(const std::filesystem::path &socketPath,
                   ControlMessageHandler handler, void *userData);

/**
 * Reports one synchronous client exchange.
 */
struct ControlExchangeResult {
  /** Response body without framing newline. */
  std::string response;
  /** Transport or peer-validation diagnostic, or empty on success. */
  std::string error;
};

/**
 * Sends one message to a same-user control server.
 *
 * @param socketPath Unix-domain endpoint path.
 * @param request Request body without framing newline.
 * @return Response body or a transport diagnostic.
 */
ControlExchangeResult
exchangeControlMessage(const std::filesystem::path &socketPath,
                       std::string_view request);

} // namespace pipetune

#endif
