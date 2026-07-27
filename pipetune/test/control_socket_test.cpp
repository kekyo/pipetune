#include "control_socket.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

struct HandlerState {
  std::mutex mutex;
  std::string request;
};

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static std::string handleRequest(std::string_view request, void *userData) {
  auto &state = *static_cast<HandlerState *>(userData);
  {
    auto lock = std::scoped_lock(state.mutex);
    state.request = request;
  }
  return R"json({"ok":true,"answer":42})json";
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

  auto started =
      pipetune::startControlServer(socketPath, handleRequest, &state);
  if (!check(started.server != nullptr, started.error)) {
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

  auto duplicate =
      pipetune::startControlServer(socketPath, handleRequest, &state);
  if (!check(duplicate.server == nullptr,
             "a second server must not replace an active socket") ||
      !check(!duplicate.error.empty(),
             "a second server must report why startup failed")) {
    started.server.reset();
    std::filesystem::remove_all(directory);
    return 1;
  }

  started.server.reset();
  const auto removed =
      check(!std::filesystem::exists(socketPath),
            "control socket must be removed when the server stops");
  std::filesystem::remove_all(directory);
  return removed ? 0 : 1;
}
