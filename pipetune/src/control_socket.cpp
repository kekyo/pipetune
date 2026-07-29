#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "pipetune/control_socket.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <optional>
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
#include <vector>

namespace pipetune {

constexpr auto kMaximumControlRequestBytes = std::size_t{64 * 1024};
constexpr auto kMaximumControlResponseBytes = std::size_t{256 * 1024};
constexpr auto kControlBacklog = 8;
constexpr auto kMaximumControlSubscribers = std::size_t{8};
constexpr auto kClientTimeoutSeconds = 15;
constexpr auto kStatusPublicationInterval = std::chrono::seconds{1};

struct ControlServer::Impl {
  std::filesystem::path socketPath;
  ControlMessageHandler handler;
  ControlStatusProvider statusProvider;
  void *userData;
  int listener;
  int stopEvent;
  int publishEvent;
  bool ownsSocket;
  std::thread thread;

  Impl(std::filesystem::path path, const ControlServerOptions &options)
      : socketPath(std::move(path)), handler(options.handler),
        statusProvider(options.statusProvider), userData(options.userData),
        listener(-1), stopEvent(-1), publishEvent(-1), ownsSocket(false),
        thread() {}

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
    if (publishEvent >= 0) {
      close(publishEvent);
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

struct ControlSubscriber {
  int descriptor;
  std::string output;
  std::size_t outputOffset;
  std::optional<std::string> pendingOutput;
};

static std::string framedMessage(std::string_view message) {
  auto framed = std::string(message);
  framed.push_back('\n');
  return framed;
}

static void closeSubscriber(ControlSubscriber &subscriber) {
  if (subscriber.descriptor >= 0) {
    close(subscriber.descriptor);
    subscriber.descriptor = -1;
  }
}

static bool configureSubscriber(int descriptor) {
  const auto flags = fcntl(descriptor, F_GETFL, 0);
  return flags >= 0 &&
         fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

static void queueSubscriberMessage(ControlSubscriber &subscriber,
                                   std::string_view message) {
  auto framed = framedMessage(message);
  if (subscriber.output.empty()) {
    subscriber.output = std::move(framed);
    subscriber.outputOffset = 0;
    return;
  }
  subscriber.pendingOutput = std::move(framed);
}

static bool flushSubscriber(ControlSubscriber &subscriber) {
  while (!subscriber.output.empty()) {
    const auto count =
        send(subscriber.descriptor,
             subscriber.output.data() + subscriber.outputOffset,
             subscriber.output.size() - subscriber.outputOffset,
             MSG_NOSIGNAL | MSG_DONTWAIT);
    if (count > 0) {
      subscriber.outputOffset += static_cast<std::size_t>(count);
      if (subscriber.outputOffset < subscriber.output.size()) {
        continue;
      }
      subscriber.output.clear();
      subscriber.outputOffset = 0;
      if (subscriber.pendingOutput.has_value()) {
        subscriber.output = std::move(*subscriber.pendingOutput);
        subscriber.pendingOutput.reset();
      }
      continue;
    }
    if (count < 0 && errno == EINTR) {
      continue;
    }
    return count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  }
  return true;
}

static void publishToSubscribers(
    ControlServer::Impl &implementation,
    std::vector<ControlSubscriber> &subscribers) {
  if (subscribers.empty() || implementation.statusProvider == nullptr) {
    return;
  }
  auto message = std::string{};
  try {
    message = implementation.statusProvider(implementation.userData);
  } catch (const std::exception &) {
    return;
  }
  if (message.empty() || message.size() > kMaximumControlResponseBytes ||
      message.find('\n') != std::string::npos) {
    return;
  }
  for (auto &subscriber : subscribers) {
    queueSubscriberMessage(subscriber, message);
  }
}

static bool handleClient(ControlServer::Impl &implementation, int descriptor,
                         std::vector<ControlSubscriber> &subscribers) {
  if (!sameUserPeer(descriptor)) {
    close(descriptor);
    return false;
  }
  auto request = std::string{};
  if (!readServerRequest(descriptor, implementation.stopEvent, request)) {
    close(descriptor);
    return false;
  }
  auto result = ControlMessageResult{
      .response = {},
      .connectionMode = ControlConnectionMode::close,
      .publishStatus = false,
  };
  try {
    result = implementation.handler(request, implementation.userData);
  } catch (const std::exception &) {
    result.response =
        R"json({"ok":false,"error":"control request handler failed"})json";
  }
  if (result.response.empty() ||
      result.response.size() > kMaximumControlResponseBytes ||
      result.response.find('\n') != std::string::npos) {
    result.response =
        R"json({"ok":false,"error":"control response is unavailable"})json";
    result.connectionMode = ControlConnectionMode::close;
    result.publishStatus = false;
  }
  if (!writeServerResponse(descriptor, implementation.stopEvent,
                           result.response)) {
    close(descriptor);
    return result.publishStatus;
  }
  if (result.connectionMode == ControlConnectionMode::subscribe &&
      implementation.statusProvider != nullptr &&
      subscribers.size() < kMaximumControlSubscribers &&
      configureSubscriber(descriptor)) {
    subscribers.push_back({.descriptor = descriptor,
                           .output = {},
                           .outputOffset = 0,
                           .pendingOutput = std::nullopt});
  } else {
    close(descriptor);
  }
  return result.publishStatus;
}

static void drainEvent(int descriptor) {
  auto value = eventfd_t{0};
  while (eventfd_read(descriptor, &value) == 0) {
  }
}

static void closeSubscribers(
    std::vector<ControlSubscriber> &subscribers) {
  for (auto &subscriber : subscribers) {
    closeSubscriber(subscriber);
  }
  subscribers.clear();
}

static void removeClosedSubscribers(
    std::vector<ControlSubscriber> &subscribers) {
  std::erase_if(subscribers, [](const ControlSubscriber &subscriber) {
    return subscriber.descriptor < 0;
  });
}

static void runControlServer(ControlServer::Impl *implementation) {
  auto subscribers = std::vector<ControlSubscriber>{};
  auto nextPublication =
      std::optional<std::chrono::steady_clock::time_point>{};
  while (true) {
    const auto beforePoll = std::chrono::steady_clock::now();
    if (subscribers.empty()) {
      nextPublication.reset();
    } else if (!nextPublication.has_value()) {
      nextPublication = beforePoll + kStatusPublicationInterval;
    }
    const auto timeout =
        nextPublication.has_value()
            ? static_cast<int>(
                  std::chrono::ceil<std::chrono::milliseconds>(
                      std::max(std::chrono::steady_clock::duration::zero(),
                               *nextPublication - beforePoll))
                      .count())
            : -1;
    auto descriptors = std::vector<pollfd>{};
    descriptors.reserve(3 + subscribers.size());
    descriptors.push_back(pollfd{.fd = implementation->listener,
                                 .events = POLLIN,
                                 .revents = 0});
    descriptors.push_back(pollfd{.fd = implementation->stopEvent,
                                 .events = POLLIN,
                                 .revents = 0});
    descriptors.push_back(pollfd{.fd = implementation->publishEvent,
                                 .events = POLLIN,
                                 .revents = 0});
    for (const auto &subscriber : subscribers) {
      auto events = static_cast<short>(POLLIN);
      if (!subscriber.output.empty()) {
        events = static_cast<short>(events | POLLOUT);
      }
      descriptors.push_back(pollfd{.fd = subscriber.descriptor,
                                   .events = events,
                                   .revents = 0});
    }

    auto result = int{-1};
    do {
      result = poll(descriptors.data(), descriptors.size(), timeout);
    } while (result < 0 && errno == EINTR);
    if (result < 0 || (descriptors[1].revents & POLLIN) != 0) {
      closeSubscribers(subscribers);
      return;
    }
    const auto afterPoll = std::chrono::steady_clock::now();
    if (nextPublication.has_value() && afterPoll >= *nextPublication) {
      publishToSubscribers(*implementation, subscribers);
      do {
        *nextPublication += kStatusPublicationInterval;
      } while (afterPoll >= *nextPublication);
    }
    if ((descriptors[2].revents & POLLIN) != 0) {
      drainEvent(implementation->publishEvent);
      publishToSubscribers(*implementation, subscribers);
    }

    for (auto index = std::size_t{0}; index < subscribers.size(); ++index) {
      const auto revents = descriptors[index + 3].revents;
      auto &subscriber = subscribers[index];
      if ((revents & static_cast<short>(POLLHUP | POLLERR | POLLNVAL |
                                       POLLIN)) != 0) {
        closeSubscriber(subscriber);
      } else if ((revents & POLLOUT) != 0 &&
                 !flushSubscriber(subscriber)) {
        closeSubscriber(subscriber);
      }
    }
    removeClosedSubscribers(subscribers);

    if ((descriptors[0].revents & POLLIN) != 0) {
      const auto client =
          accept4(implementation->listener, nullptr, nullptr, SOCK_CLOEXEC);
      if (client >= 0) {
        if (handleClient(*implementation, client, subscribers)) {
          publishToSubscribers(*implementation, subscribers);
        }
      } else if (errno != EINTR && errno != EAGAIN &&
                 errno != EWOULDBLOCK) {
        closeSubscribers(subscribers);
        return;
      }
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
                   const ControlServerOptions &options) {
  if (options.handler == nullptr) {
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
      std::make_unique<ControlServer::Impl>(socketPath, options);
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
  implementation->publishEvent = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (implementation->publishEvent < 0) {
    return {.server = nullptr,
            .error = socketError("cannot create control publish event")};
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

void publishControlStatus(ControlServer *server) {
  if (server == nullptr || server->implementation_ == nullptr ||
      server->implementation_->publishEvent < 0) {
    return;
  }
  const auto wake = eventfd_t{1};
  if (eventfd_write(server->implementation_->publishEvent, wake) != 0 &&
      errno != EAGAIN) {
    return;
  }
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
