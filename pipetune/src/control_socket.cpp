/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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
  int eventDescriptor;
  ControlDescriptorEventHandler eventHandler;
  AsyncControlMessageHandler asyncHandler;
  AsyncControlDescriptorEventHandler asyncEventHandler;
  AsyncControlStatusProvider asyncStatusProvider;
  int listener;
  int stopEvent;
  int publishEvent;
  bool ownsSocket;
  std::thread thread;

  Impl(std::filesystem::path path, const ControlServerOptions &options)
      : socketPath(std::move(path)), handler(options.handler),
        statusProvider(options.statusProvider), userData(options.userData),
        eventDescriptor(options.eventDescriptor),
        eventHandler(options.eventHandler), asyncHandler(options.asyncHandler),
        asyncEventHandler(options.asyncEventHandler), asyncStatusProvider(options.asyncStatusProvider), listener(-1), stopEvent(-1),
        publishEvent(-1), ownsSocket(false), thread() {}

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

struct ControlSession {
  int descriptor;
  bool subscriber = false;
  std::optional<std::string> pendingOutput;
  cardio::primitives::conditional publication;
  cardio::cancellation_source cancellation;

  explicit ControlSession(int value) : descriptor(value) {}
  ~ControlSession() { close(descriptor); }
};

struct ControlService {
  ControlServer::Impl &implementation;
  cardio::cancellation_source cancellation;
  struct Job {
    std::shared_ptr<ControlSession> session;
    cardio::promise<void> completion;
  };
  std::vector<Job> jobs;
};

static void drainEvent(int descriptor) {
  auto value = eventfd_t{0};
  while (eventfd_read(descriptor, &value) == 0) {}
}

static void collectCompletedClients(ControlService &service) {
  std::erase_if(service.jobs, [](const auto &job) { return job.completion.try_result(); });
}

static cardio::promise<void> publishToSubscribers(ControlService &service) {
  if (service.implementation.statusProvider == nullptr && service.implementation.asyncStatusProvider == nullptr) co_return;
  auto message = std::string{};
  try {
    if (service.implementation.asyncStatusProvider)
      message = std::move(co_await service.implementation.asyncStatusProvider(
          service.implementation.userData, service.cancellation.get_cancellation()));
    else message = service.implementation.statusProvider(service.implementation.userData);
  } catch (const std::exception &) { co_return; }
  if (message.empty() || message.size() > kMaximumControlResponseBytes ||
      message.find('\n') != std::string::npos) co_return;
  for (auto &job : service.jobs) {
    if (!job.session->subscriber) continue;
    // Retain only the latest unsent status while a slow subscriber drains its
    // current message. A slow peer cannot block other clients or grow a queue.
    job.session->pendingOutput = message;
    job.session->publication.trigger();
  }
}

static cardio::promise<std::string> readServerRequest(
    int descriptor, cardio::cancellation cancellation) {
  auto request = std::string{};
  auto buffer = std::array<char, 4096>{};
  for (;;) {
    co_await cardio::from_fd(descriptor, cardio::fd_event::read, cancellation);
    const auto count = recv(descriptor, buffer.data(), buffer.size(), MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    if (count <= 0) throw std::runtime_error("control peer disconnected");
    request.append(buffer.data(), static_cast<std::size_t>(count));
    const auto newline = request.find('\n');
    if (newline != std::string::npos && newline <= kMaximumControlRequestBytes) {
      request.resize(newline);
      co_return request;
    }
    if (request.size() > kMaximumControlRequestBytes)
      throw std::runtime_error("control request exceeds its byte limit");
  }
}

static cardio::promise<void> writeServerResponse(
    int descriptor, std::string_view response, cardio::cancellation cancellation) {
  auto framed = std::string(response);
  framed.push_back('\n');
  auto written = std::size_t{0};
  while (written < framed.size()) {
    co_await cardio::from_fd(descriptor, cardio::fd_event::write, cancellation);
    const auto count = send(descriptor, framed.data() + written, framed.size() - written,
                            MSG_NOSIGNAL | MSG_DONTWAIT);
    if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
    if (count <= 0) throw std::runtime_error("control peer stopped reading");
    written += static_cast<std::size_t>(count);
  }
}

static cardio::promise<void> observeDisconnect(ControlSession &session) {
  try {
    co_await cardio::from_fd(session.descriptor, cardio::fd_event::read,
                            session.cancellation.get_cancellation());
    // The protocol accepts exactly one request per connection.
    session.cancellation.cancel();
  } catch (const cardio::canceled_exception &) {}
}

static cardio::promise<void> handleClient(ControlService &service,
                                          std::shared_ptr<ControlSession> session) {
  auto registration = service.cancellation.get_cancellation().on_cancellation_requested(
      [session] { session->cancellation.cancel(); });
  const auto cancellation = session->cancellation.get_cancellation();
  auto disconnected = std::optional<cardio::promise<void>>{};
  try {
    auto request = std::move(co_await readServerRequest(session->descriptor, cancellation));
    disconnected.emplace(observeDisconnect(*session));
    auto &implementation = service.implementation;
    auto result = ControlMessageResult{{}, ControlConnectionMode::close, false};
    try {
      if (implementation.asyncHandler) {
        result = std::move(co_await implementation.asyncHandler(request, implementation.userData, cancellation));
      } else {
        result = implementation.handler(request, implementation.userData);
      }
    } catch (const cardio::canceled_exception &) { throw; }
    catch (const std::exception &) {
      result.response = R"json({"ok":false,"error":"control request handler failed"})json";
    }
    cancellation.throw_if_cancellation_requested();
    if (result.response.empty() || result.response.size() > kMaximumControlResponseBytes ||
        result.response.find('\n') != std::string::npos) {
      result = {R"json({"ok":false,"error":"control response is unavailable"})json",
                ControlConnectionMode::close, false};
    }
    if (result.publishStatus) co_await publishToSubscribers(service);
    co_await writeServerResponse(session->descriptor, result.response, cancellation);
    const auto subscriberCount = std::ranges::count_if(service.jobs,
        [](const auto &job) { return job.session->subscriber; });
    if (result.connectionMode == ControlConnectionMode::subscribe &&
        (implementation.statusProvider || implementation.asyncStatusProvider) &&
        subscriberCount < static_cast<std::ptrdiff_t>(kMaximumControlSubscribers)) {
      session->subscriber = true;
      for (;;) {
        if (!session->pendingOutput) co_await session->publication.wait(cancellation);
        cancellation.throw_if_cancellation_requested();
        if (!session->pendingOutput) continue;
        auto output = std::move(*session->pendingOutput);
        session->pendingOutput.reset();
        co_await writeServerResponse(session->descriptor, output, cancellation);
      }
    }
  } catch (const std::exception &) {}
  session->subscriber = false;
  session->cancellation.cancel();
  if (disconnected) co_await *disconnected;
  // Keep descriptor ownership in this coroutine until every readiness wait has
  // completed. A later connection must never inherit a still-observed fd number.
  shutdown(session->descriptor, SHUT_RDWR);
}

static cardio::promise<void> acceptClients(ControlService &service) {
  const auto cancellation = service.cancellation.get_cancellation();
  try {
    for (;;) {
      co_await cardio::from_fd(service.implementation.listener, cardio::fd_event::read, cancellation);
      const auto descriptor = accept4(service.implementation.listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
      if (descriptor < 0) {
        if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
        throw std::runtime_error(socketError("cannot accept control connection"));
      }
      collectCompletedClients(service);
      if (!sameUserPeer(descriptor) || service.jobs.size() >= 64) {
        close(descriptor);
        continue;
      }
      auto session = std::make_shared<ControlSession>(descriptor);
      auto completion = handleClient(service, session);
      service.jobs.push_back({std::move(session), std::move(completion)});
    }
  } catch (const cardio::canceled_exception &) {}
}

static cardio::promise<void> observePublication(ControlService &service) {
  const auto cancellation = service.cancellation.get_cancellation();
  try {
    for (;;) {
      co_await cardio::from_fd(service.implementation.publishEvent, cardio::fd_event::read, cancellation);
      drainEvent(service.implementation.publishEvent);
      collectCompletedClients(service);
      co_await publishToSubscribers(service);
    }
  } catch (const cardio::canceled_exception &) {}
}

static cardio::promise<void> publishPeriodically(ControlService &service) {
  try {
    for (;;) {
      co_await cardio::promises::delay(
          std::chrono::duration_cast<std::chrono::milliseconds>(kStatusPublicationInterval).count(),
          service.cancellation.get_cancellation());
      collectCompletedClients(service);
      if (std::ranges::any_of(service.jobs, [](const auto &job) { return job.session->subscriber; }))
        co_await publishToSubscribers(service);
    }
  } catch (const cardio::canceled_exception &) {}
}

static cardio::promise<void> observeApplicationEvents(ControlService &service) {
  auto &implementation = service.implementation;
  if (implementation.eventDescriptor < 0) co_return;
  const auto cancellation = service.cancellation.get_cancellation();
  try {
    for (;;) {
      co_await cardio::from_fd(implementation.eventDescriptor, cardio::fd_event::read, cancellation);
      auto publish = false;
      try {
        if (implementation.asyncEventHandler)
          publish = co_await implementation.asyncEventHandler(implementation.userData, cancellation);
        else publish = implementation.eventHandler(implementation.userData);
      } catch (const cardio::canceled_exception &) { cancellation.throw_if_cancellation_requested(); }
      catch (const std::exception &) {}
      if (publish) co_await publishToSubscribers(service);
    }
  } catch (const cardio::canceled_exception &) {}
}

static cardio::promise<void> serve(ControlServer::Impl &implementation,
                                    cardio::dispatcher_group_glib &group) {
  auto service = ControlService{implementation, {}, {}};
  auto accepting = acceptClients(service);
  auto publication = observePublication(service);
  auto periodic = publishPeriodically(service);
  auto application = observeApplicationEvents(service);
  co_await cardio::from_fd(implementation.stopEvent, cardio::fd_event::read);
  service.cancellation.cancel();
  co_await accepting;
  co_await publication;
  co_await periodic;
  co_await application;
  for (auto &job : service.jobs) co_await job.completion;
  group.shutdown();
}

static void runControlServer(ControlServer::Impl *implementation) {
  const auto context = std::unique_ptr<GMainContext, decltype(&g_main_context_unref)>(
      g_main_context_new(), g_main_context_unref);
  g_main_context_push_thread_default(context.get());
  {
    cardio::dispatcher_group_glib group(context.get());
    cardio::dispatcher_host_glib host(group);
    const auto operation = serve(*implementation, group);
    host.park();
  }
  g_main_context_pop_thread_default(context.get());
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
  if (options.statusProvider && options.asyncStatusProvider)
    return {.server = nullptr, .error = "configure only one control status provider"};
  if ((options.handler == nullptr) == (options.asyncHandler == nullptr)) {
    return {.server = nullptr,
            .error = "exactly one control message handler is required"};
  }
  if ((options.eventDescriptor >= 0) !=
      (options.eventHandler != nullptr || options.asyncEventHandler != nullptr) ||
      (options.eventHandler != nullptr && options.asyncEventHandler != nullptr)) {
    return {.server = nullptr,
            .error = "control event descriptor and handler must be paired"};
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
      socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
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
    // Only a refused or missing endpoint proves that no request was sent.
    // Timeouts and failures after connecting may hide an applied change.
    const auto unavailable =
        error.empty() && (errno == ENOENT || errno == ECONNREFUSED);
    if (error.empty()) {
      error = socketError("cannot connect to PipeTune control socket");
    }
    close(descriptor);
    return {.response = {}, .error = std::move(error),
            .unavailable = unavailable};
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
