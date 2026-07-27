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
 * Selects the lifetime of a handled control connection.
 */
enum class ControlConnectionMode {
  /** Close the connection after sending the response. */
  close,
  /** Keep the connection open for status publications. */
  subscribe
};

/**
 * Describes the result of handling one control message.
 */
struct ControlMessageResult {
  /** Response body without framing newline. */
  std::string response;
  /** Whether the connection becomes a subscriber. */
  ControlConnectionMode connectionMode;
  /** Whether all subscribers should receive a fresh status. */
  bool publishStatus;
};

/**
 * Handles one complete control message outside the audio thread.
 *
 * @param request Message body without framing newline.
 * @param userData Opaque pointer supplied to startControlServer().
 * @return Response and connection behavior.
 */
using ControlMessageHandler =
    ControlMessageResult (*)(std::string_view request, void *userData);

/**
 * Produces a fresh status event on the control service thread.
 *
 * @param userData Opaque pointer supplied to startControlServer().
 * @return Event body without framing newline.
 */
using ControlStatusProvider = std::string (*)(void *userData);

/**
 * Configures callbacks for a control server.
 */
struct ControlServerOptions {
  /** Non-null request callback. */
  ControlMessageHandler handler;
  /** Status callback required when subscriptions are accepted. */
  ControlStatusProvider statusProvider;
  /** Opaque argument passed to both callbacks. */
  void *userData;
};

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
                     const ControlServerOptions &options);
  friend void publishControlStatus(ControlServer *server);
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
 * @param options Server callbacks and their opaque argument.
 * @return Running server or a startup diagnostic.
 */
ControlServerStartResult
startControlServer(const std::filesystem::path &socketPath,
                   const ControlServerOptions &options);

/**
 * Coalesces a request to publish fresh status to all subscribers.
 *
 * The configured status provider runs later on the control service thread.
 * A null server is ignored.
 *
 * @param server Running server, or null.
 */
void publishControlStatus(ControlServer *server);

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
