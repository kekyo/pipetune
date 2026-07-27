#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "control_socket.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <poll.h>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>
#include <utility>

namespace pipetune {

constexpr auto kMaximumControlRequestBytes = std::size_t{64 * 1024};
constexpr auto kMaximumControlResponseBytes = std::size_t{256 * 1024};
constexpr auto kControlBacklog = 8;
constexpr auto kClientTimeoutSeconds = 5;

struct ControlServer::Impl {
  std::filesystem::path socketPath;
  ControlMessageHandler handler;
  void *userData;
  int listener;
  int stopEvent;
  bool ownsSocket;
  std::thread thread;

  Impl(std::filesystem::path path, ControlMessageHandler messageHandler,
       void *messageUserData)
      : socketPath(std::move(path)), handler(messageHandler),
        userData(messageUserData), listener(-1), stopEvent(-1),
        ownsSocket(false), thread() {}

  ~Impl() {
    if (thread.joinable()) {
      const auto wake = eventfd_t{1};
      static_cast<void>(eventfd_write(stopEvent, wake));
      thread.join();
    }
    if (listener >= 0) {
      close(listener);
    }
    if (stopEvent >= 0) {
      close(stopEvent);
    }
    if (ownsSocket) {
      unlink(socketPath.c_str());
    }
  }
};

static std::string socketError(std::string_view operation) {
  return std::string(operation) + ": " + std::strerror(errno);
}

static bool makeSocketAddress(const std::filesystem::path &path,
                              sockaddr_un &address, socklen_t &length,
                              std::string &error) {
  const auto native = path.string();
  if (native.empty()) {
    error = "control socket path must not be empty";
    return false;
  }
  if (native.find('\0') != std::string::npos) {
    error = "control socket path must not contain NUL";
    return false;
  }
  if (native.size() >= sizeof(address.sun_path)) {
    error = "control socket path is too long";
    return false;
  }
  address = sockaddr_un{};
  address.sun_family = AF_UNIX;
  std::memcpy(address.sun_path, native.c_str(), native.size() + 1);
  length = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) +
                                  native.size() + 1);
  return true;
}

static bool sameUserPeer(int descriptor) {
  auto credentials = ucred{};
  auto length = socklen_t{sizeof(credentials)};
  return getsockopt(descriptor, SOL_SOCKET, SO_PEERCRED, &credentials,
                    &length) == 0 &&
         length == sizeof(credentials) && credentials.uid == geteuid();
}

static bool socketIsActive(const sockaddr_un &address, socklen_t length) {
  const auto descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return true;
  }
  const auto result =
      connect(descriptor, reinterpret_cast<const sockaddr *>(&address), length);
  const auto savedError = errno;
  close(descriptor);
  errno = savedError;
  return result == 0 || savedError != ECONNREFUSED;
}

static bool removeStaleSocket(const std::filesystem::path &path,
                              const sockaddr_un &address, socklen_t length,
                              std::string &error) {
  struct stat status {};
  if (lstat(path.c_str(), &status) != 0) {
    error = socketError("cannot inspect existing control socket");
    return false;
  }
  if (!S_ISSOCK(status.st_mode) || status.st_uid != geteuid()) {
    error = "control socket path is occupied by an unsafe existing object";
    return false;
  }
  if (socketIsActive(address, length)) {
    error = "another PipeTune control server is already running";
    return false;
  }
  if (unlink(path.c_str()) != 0 && errno != ENOENT) {
    error = socketError("cannot remove stale control socket");
    return false;
  }
  return true;
}

static bool bindListener(ControlServer::Impl &implementation,
                         const sockaddr_un &address, socklen_t length,
                         std::string &error) {
  if (bind(implementation.listener,
           reinterpret_cast<const sockaddr *>(&address), length) != 0) {
    if (errno != EADDRINUSE ||
        !removeStaleSocket(implementation.socketPath, address, length, error)) {
      if (error.empty()) {
        error = socketError("cannot bind control socket");
      }
      return false;
    }
    if (bind(implementation.listener,
             reinterpret_cast<const sockaddr *>(&address), length) != 0) {
      error = socketError("cannot bind recovered control socket");
      return false;
    }
  }
  implementation.ownsSocket = true;
  if (chmod(implementation.socketPath.c_str(), 0600) != 0) {
    error = socketError("cannot secure control socket");
    return false;
  }
  if (listen(implementation.listener, kControlBacklog) != 0) {
    error = socketError("cannot listen on control socket");
    return false;
  }
  return true;
}

static bool waitForDescriptor(int descriptor, short events, int stopEvent) {
  auto descriptors = std::array<pollfd, 2>{
      pollfd{.fd = descriptor, .events = events, .revents = 0},
      pollfd{.fd = stopEvent, .events = POLLIN, .revents = 0}};
  auto result = int{-1};
  do {
    result = poll(descriptors.data(), descriptors.size(), -1);
  } while (result < 0 && errno == EINTR);
  return result > 0 && (descriptors[1].revents & POLLIN) == 0 &&
         (descriptors[0].revents &
          static_cast<short>(events | POLLHUP | POLLERR)) != 0;
}

static bool readServerRequest(int descriptor, int stopEvent,
                              std::string &request) {
  auto buffer = std::array<char, 4096>{};
  while (request.size() <= kMaximumControlRequestBytes) {
    if (!waitForDescriptor(descriptor, POLLIN, stopEvent)) {
      return false;
    }
    auto count = ssize_t{-1};
    do {
      count = recv(descriptor, buffer.data(), buffer.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      return false;
    }
    request.append(buffer.data(), static_cast<std::size_t>(count));
    const auto newline = request.find('\n');
    if (newline != std::string::npos) {
      request.resize(newline);
      return true;
    }
  }
  return false;
}

static bool writeServerResponse(int descriptor, int stopEvent,
                                std::string_view response) {
  auto framed = std::string(response);
  framed.push_back('\n');
  auto written = std::size_t{0};
  while (written < framed.size()) {
    if (!waitForDescriptor(descriptor, POLLOUT, stopEvent)) {
      return false;
    }
    auto count = ssize_t{-1};
    do {
      count = send(descriptor, framed.data() + written,
                   framed.size() - written, MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      return false;
    }
    written += static_cast<std::size_t>(count);
  }
  return true;
}

static void handleClient(ControlServer::Impl &implementation,
                         int descriptor) {
  if (!sameUserPeer(descriptor)) {
    close(descriptor);
    return;
  }
  auto request = std::string{};
  if (!readServerRequest(descriptor, implementation.stopEvent, request)) {
    close(descriptor);
    return;
  }
  auto response = std::string{};
  try {
    response = implementation.handler(request, implementation.userData);
  } catch (const std::exception &) {
    response =
        R"json({"ok":false,"error":"control request handler failed"})json";
  }
  if (response.empty() || response.size() > kMaximumControlResponseBytes) {
    response =
        R"json({"ok":false,"error":"control response is unavailable"})json";
  }
  static_cast<void>(
      writeServerResponse(descriptor, implementation.stopEvent, response));
  close(descriptor);
}

static void runControlServer(ControlServer::Impl *implementation) {
  auto descriptors = std::array<pollfd, 2>{
      pollfd{.fd = implementation->listener,
             .events = POLLIN,
             .revents = 0},
      pollfd{.fd = implementation->stopEvent,
             .events = POLLIN,
             .revents = 0}};
  while (true) {
    auto result = int{-1};
    do {
      result = poll(descriptors.data(), descriptors.size(), -1);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || (descriptors[1].revents & POLLIN) != 0) {
      return;
    }
    if ((descriptors[0].revents & POLLIN) == 0) {
      continue;
    }
    const auto client =
        accept4(implementation->listener, nullptr, nullptr, SOCK_CLOEXEC);
    if (client >= 0) {
      handleClient(*implementation, client);
    } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
      return;
    }
  }
}

ControlServer::ControlServer(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

ControlServer::~ControlServer() = default;

ControlSocketPathResult
resolveControlSocketPath(const std::filesystem::path &configuredPath) {
  if (!configuredPath.empty()) {
    return {.path = configuredPath, .error = {}};
  }
  const auto *runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
  if (runtimeDirectory == nullptr || runtimeDirectory[0] == '\0') {
    return {.path = {},
            .error = "XDG_RUNTIME_DIR is required for the default control "
                     "socket"};
  }
  return {.path = std::filesystem::path(runtimeDirectory) / "pipetune" /
                  "control.sock",
          .error = {}};
}

ControlServerStartResult
startControlServer(const std::filesystem::path &socketPath,
                   ControlMessageHandler handler, void *userData) {
  if (handler == nullptr) {
    return {.server = nullptr,
            .error = "control message handler must not be null"};
  }
  auto address = sockaddr_un{};
  auto addressLength = socklen_t{0};
  auto error = std::string{};
  if (!makeSocketAddress(socketPath, address, addressLength, error)) {
    return {.server = nullptr, .error = std::move(error)};
  }
  auto filesystemError = std::error_code{};
  const auto parent = socketPath.parent_path();
  if (!parent.empty()) {
    const auto created =
        std::filesystem::create_directories(parent, filesystemError);
    if (filesystemError) {
      return {.server = nullptr,
              .error = "cannot create control socket directory: " +
                       filesystemError.message()};
    }
    if (created && chmod(parent.c_str(), 0700) != 0) {
      return {.server = nullptr,
              .error = socketError("cannot secure control socket directory")};
    }
  }

  auto implementation =
      std::make_unique<ControlServer::Impl>(socketPath, handler, userData);
  implementation->listener =
      socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (implementation->listener < 0) {
    return {.server = nullptr,
            .error = socketError("cannot create control socket")};
  }
  implementation->stopEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (implementation->stopEvent < 0) {
    return {.server = nullptr,
            .error = socketError("cannot create control stop event")};
  }
  if (!bindListener(*implementation, address, addressLength, error)) {
    return {.server = nullptr, .error = std::move(error)};
  }
  try {
    implementation->thread =
        std::thread(runControlServer, implementation.get());
  } catch (const std::exception &exception) {
    return {.server = nullptr,
            .error = "cannot start control server thread: " +
                     std::string(exception.what())};
  }
  return {.server = std::unique_ptr<ControlServer>(
              new ControlServer(std::move(implementation))),
          .error = {}};
}

static bool configureClientTimeouts(int descriptor, std::string &error) {
  auto timeout = timeval{.tv_sec = kClientTimeoutSeconds, .tv_usec = 0};
  if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0 ||
      setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    error = socketError("cannot configure control client timeout");
    return false;
  }
  return true;
}

ControlExchangeResult
exchangeControlMessage(const std::filesystem::path &socketPath,
                       std::string_view request) {
  if (request.empty() || request.size() > kMaximumControlRequestBytes ||
      request.find('\n') != std::string_view::npos) {
    return {.response = {},
            .error = "control request must be one non-empty JSON line"};
  }
  auto address = sockaddr_un{};
  auto addressLength = socklen_t{0};
  auto error = std::string{};
  if (!makeSocketAddress(socketPath, address, addressLength, error)) {
    return {.response = {}, .error = std::move(error)};
  }

  const auto descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return {.response = {},
            .error = socketError("cannot create control client socket")};
  }
  if (!configureClientTimeouts(descriptor, error) ||
      connect(descriptor, reinterpret_cast<const sockaddr *>(&address),
              addressLength) != 0) {
    if (error.empty()) {
      error = socketError("cannot connect to PipeTune control socket");
    }
    close(descriptor);
    return {.response = {}, .error = std::move(error)};
  }
  if (!sameUserPeer(descriptor)) {
    close(descriptor);
    return {.response = {},
            .error = "PipeTune control socket belongs to another user"};
  }

  auto framed = std::string(request);
  framed.push_back('\n');
  auto sent = std::size_t{0};
  while (sent < framed.size()) {
    auto count = ssize_t{-1};
    do {
      count = send(descriptor, framed.data() + sent, framed.size() - sent,
                   MSG_NOSIGNAL);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      error = socketError("cannot send PipeTune control request");
      close(descriptor);
      return {.response = {}, .error = std::move(error)};
    }
    sent += static_cast<std::size_t>(count);
  }

  auto response = std::string{};
  auto buffer = std::array<char, 4096>{};
  while (response.size() <= kMaximumControlResponseBytes) {
    auto count = ssize_t{-1};
    do {
      count = recv(descriptor, buffer.data(), buffer.size(), 0);
    } while (count < 0 && errno == EINTR);
    if (count <= 0) {
      error = count == 0 ? "PipeTune control socket closed without a response"
                         : socketError("cannot receive PipeTune control response");
      close(descriptor);
      return {.response = {}, .error = std::move(error)};
    }
    response.append(buffer.data(), static_cast<std::size_t>(count));
    const auto newline = response.find('\n');
    if (newline != std::string::npos) {
      response.resize(newline);
      close(descriptor);
      return {.response = std::move(response), .error = {}};
    }
  }
  close(descriptor);
  return {.response = {}, .error = "PipeTune control response is too large"};
}

} // namespace pipetune
