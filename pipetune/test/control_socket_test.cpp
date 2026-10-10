/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/control_socket.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

struct HandlerState {
  std::mutex mutex;
  std::string request;
  std::string publication;
};

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlMessageResult
handleRequest(std::string_view request, void *userData) {
  auto &state = *static_cast<HandlerState *>(userData);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.request = request;
  }
  if (request == R"json({"command":"subscribe"})json") {
    return {.response = R"json({"event":"status","sequence":1})json",
            .connectionMode =
                pipetune::ControlConnectionMode::subscribe,
            .publishStatus = false};
  }
  return {.response = R"json({"ok":true,"answer":42})json",
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = false};
}

static std::string provideStatus(void *userData) {
  auto &state = *static_cast<HandlerState *>(userData);
  auto lock = std::scoped_lock(state.mutex);
  return state.publication;
}

static int connectToSocket(const std::filesystem::path &path) {
  const auto descriptor = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    return -1;
  }
  auto address = sockaddr_un{};
  address.sun_family = AF_UNIX;
  const auto native = path.string();
  if (native.size() >= sizeof(address.sun_path)) {
    close(descriptor);
    return -1;
  }
  std::copy(native.begin(), native.end(), address.sun_path);
  address.sun_path[native.size()] = '\0';
  const auto length = static_cast<socklen_t>(
      offsetof(sockaddr_un, sun_path) + native.size() + 1);
  if (connect(descriptor, reinterpret_cast<const sockaddr *>(&address),
              length) != 0) {
    close(descriptor);
    return -1;
  }
  const auto timeout = timeval{.tv_sec = 5, .tv_usec = 0};
  if (setsockopt(descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                 sizeof(timeout)) != 0 ||
      setsockopt(descriptor, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                 sizeof(timeout)) != 0) {
    close(descriptor);
    return -1;
  }
  return descriptor;
}

static bool writeLine(int descriptor, std::string_view line) {
  auto framed = std::string(line);
  framed.push_back('\n');
  auto offset = std::size_t{0};
  while (offset < framed.size()) {
    const auto count =
        send(descriptor, framed.data() + offset, framed.size() - offset,
             MSG_NOSIGNAL);
    if (count <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(count);
  }
  return true;
}

static std::optional<std::string> readLine(int descriptor) {
  auto result = std::string{};
  while (result.size() <= 256U * 1024U) {
    auto byte = char{0};
    const auto count = recv(descriptor, &byte, 1, 0);
    if (count <= 0) {
      return std::nullopt;
    }
    if (byte == '\n') {
      return result;
    }
    result.push_back(byte);
  }
  return std::nullopt;
}

static bool testSocketPathResolution() {
  const auto *current = std::getenv("XDG_RUNTIME_DIR");
  const auto saved =
      current == nullptr ? std::optional<std::string>{}
                         : std::optional<std::string>{current};
  unsetenv("XDG_RUNTIME_DIR");
  const auto explicitPath =
      pipetune::resolveControlSocketPath("/tmp/explicit.sock");
  const auto missingDefault = pipetune::resolveControlSocketPath({});
  setenv("XDG_RUNTIME_DIR", "/tmp/pipetune-runtime", 1);
  const auto runtimeDefault = pipetune::resolveControlSocketPath({});
  if (saved.has_value()) {
    setenv("XDG_RUNTIME_DIR", saved->c_str(), 1);
  } else {
    unsetenv("XDG_RUNTIME_DIR");
  }

  return check(explicitPath.error.empty() &&
                   explicitPath.path == "/tmp/explicit.sock",
               "explicit control socket resolution differs") &&
         check(!missingDefault.error.empty(),
               "missing XDG_RUNTIME_DIR must reject an implicit socket") &&
         check(runtimeDefault.error.empty() &&
                   runtimeDefault.path ==
                       "/tmp/pipetune-runtime/pipetune/control.sock",
               "XDG control socket resolution differs");
}

struct AsyncHandlerState {
  int entered = eventfd(0, EFD_CLOEXEC);
  int release = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  std::atomic<bool> completed{false};
  std::atomic<bool> canceled{false};
  ~AsyncHandlerState() { close(entered); close(release); }
};

static cardio::promise<pipetune::ControlMessageResult> handleAsyncRequest(
    std::string_view request, void *data, cardio::cancellation cancellation) {
  auto &state = *static_cast<AsyncHandlerState *>(data);
  if (request == "hold") {
    static_cast<void>(eventfd_write(state.entered, 1));
    try {
      co_await cardio::from_fd(state.release, cardio::fd_event::read, cancellation);
      auto value = eventfd_t{0};
      static_cast<void>(eventfd_read(state.release, &value));
      state.completed = true;
    } catch (const cardio::canceled_exception &) {
      state.canceled = true;
      throw;
    }
  }
  co_return pipetune::ControlMessageResult{
      std::string(request), pipetune::ControlConnectionMode::close, false};
}

static bool testAsyncPreparation(const std::filesystem::path &path) {
  auto state = AsyncHandlerState{};
  auto server = pipetune::startControlServer(path,
      {.handler = nullptr, .statusProvider = nullptr, .userData = &state,
       .asyncHandler = handleAsyncRequest});
  if (!check(server.server != nullptr, server.error)) return false;
  const auto pending = connectToSocket(path);
  static_cast<void>(writeLine(pending, "hold"));
  auto value = eventfd_t{0};
  static_cast<void>(eventfd_read(state.entered, &value));
  const auto status = pipetune::exchangeControlMessage(path, "status");
  const auto responsive = check(status.error.empty() && status.response == "status" && !state.completed,
      "a suspended asynchronous preparation blocked an independent status request");
  static_cast<void>(eventfd_write(state.release, 1));
  const auto loaded = readLine(pending);
  close(pending);
  if (!responsive || !check(loaded == "hold" && state.completed, "suspended preparation did not resume"))
    return false;

  const auto canceled = connectToSocket(path);
  static_cast<void>(writeLine(canceled, "hold"));
  static_cast<void>(eventfd_read(state.entered, &value));
  server.server.reset();
  const auto closed = !readLine(canceled).has_value();
  close(canceled);
  return check(state.canceled && closed, "shutdown did not cancel and drain an in-flight handler");
}

int main() {
  if (!testSocketPathResolution()) {
    return 1;
  }
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-control-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto socketPath = directory / "control.sock";
  auto state = HandlerState{};
  {
    auto lock = std::scoped_lock(state.mutex);
    state.publication = R"json({"event":"status","sequence":2})json";
  }
  const auto options = pipetune::ControlServerOptions{
      .handler = handleRequest,
      .statusProvider = provideStatus,
      .userData = &state,
  };

  auto started = pipetune::startControlServer(socketPath, options);
  if (!check(started.server != nullptr, started.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  // One incomplete request must not prevent other clients from obtaining status.
  // Complete it after the second reply so failures cannot strand shutdown.
  const auto incomplete = connectToSocket(socketPath);
  const auto concurrent = connectToSocket(socketPath);
  const auto partial = std::string_view("{\"command\":");
  const auto sent = send(incomplete, partial.data(), partial.size(), MSG_NOSIGNAL);
  const auto requested = writeLine(concurrent, R"json({"command":"status"})json");
  const auto concurrentReply = readLine(concurrent);
  static_cast<void>(writeLine(incomplete, R"json("status"})json"));
  static_cast<void>(readLine(incomplete));
  close(incomplete);
  close(concurrent);
  if (!check(sent == static_cast<ssize_t>(partial.size()) && requested &&
             concurrentReply == R"json({"ok":true,"answer":42})json",
             "an incomplete request blocked an independent status request")) {
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  struct stat socketStatus {};
  if (!check(stat(socketPath.c_str(), &socketStatus) == 0,
             "control socket was not created") ||
      !check(S_ISSOCK(socketStatus.st_mode),
             "control endpoint must be a Unix socket") ||
      !check((socketStatus.st_mode & 0777) == 0600,
             "control socket must be accessible only by its owner")) {
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  const auto subscriber = connectToSocket(socketPath);
  if (!check(subscriber >= 0, "cannot connect control subscriber") ||
      !check(writeLine(subscriber,
                       R"json({"command":"subscribe"})json"),
             "cannot send subscribe request")) {
    if (subscriber >= 0) {
      close(subscriber);
    }
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }
  const auto initial = readLine(subscriber);
  if (!check(initial.has_value(),
             "subscribe request did not receive an initial status") ||
      !check(initial.value_or("") ==
                 R"json({"event":"status","sequence":1})json",
             "initial subscription status differs")) {
    close(subscriber);
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  const auto exchange =
      pipetune::exchangeControlMessage(socketPath, R"json({"command":"status"})json");
  if (!check(exchange.error.empty(), exchange.error) ||
      !check(exchange.response == R"json({"ok":true,"answer":42})json",
             "control response differs")) {
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    if (!check(state.request == R"json({"command":"status"})json",
               "control request differs")) {
      started.server.reset();
      std::filesystem::remove_all(directory);
      return 1;
    }
  }

  pipetune::publishControlStatus(started.server.get());
  const auto publication = readLine(subscriber);
  if (!check(publication.has_value(),
             "subscriber did not receive a status publication") ||
      !check(publication.value_or("") ==
                 R"json({"event":"status","sequence":2})json",
             "published subscription status differs")) {
    close(subscriber);
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  const auto periodicPublication = readLine(subscriber);
  if (!check(periodicPublication.has_value(),
             "subscriber did not receive a periodic status publication") ||
      !check(periodicPublication.value_or("") ==
                 R"json({"event":"status","sequence":2})json",
             "periodic subscription status differs")) {
    close(subscriber);
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto duplicate = pipetune::startControlServer(socketPath, options);
  if (!check(duplicate.server == nullptr,
             "a second server must not replace an active socket") ||
      !check(!duplicate.error.empty(),
             "a second server must report why startup failed")) {
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  started.server.reset();
  const auto subscriberClosed =
      check(!readLine(subscriber).has_value(),
            "server shutdown must close subscription connections");
  close(subscriber);
  const auto removed =
      check(!std::filesystem::exists(socketPath),
            "control socket must be removed when the server stops");
  const auto asynchronous = testAsyncPreparation(socketPath);
  std::filesystem::remove_all(directory);
  return removed && subscriberClosed && asynchronous ? 0 : 1;
}
