#include "default_sink_restore.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool pipeWireSessionIsAvailable() {
  const auto *runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
  if (runtimeDirectory == nullptr || runtimeDirectory[0] == '\0') {
    return false;
  }
  const auto *configuredRemote = std::getenv("PIPEWIRE_REMOTE");
  const auto remote =
      configuredRemote == nullptr || configuredRemote[0] == '\0'
          ? std::filesystem::path("pipewire-0")
          : std::filesystem::path(configuredRemote);
  const auto socket = remote.is_absolute()
                          ? remote
                          : std::filesystem::path(runtimeDirectory) / remote;
  return std::filesystem::exists(socket);
}

int main() {
  const auto encoded =
      pipetune::makeDefaultSinkMetadataValue("speaker \"main\"");
  if (!check(!encoded.empty(), "default sink metadata encoding failed") ||
      !check(pipetune::defaultSinkNameFromMetadata(encoded.c_str()) ==
                 "speaker \"main\"",
             "default sink metadata roundtrip differs") ||
      !check(pipetune::defaultSinkNameFromMetadata("not-json").empty(),
             "invalid default sink metadata must be rejected")) {
    return 1;
  }
  if (!pipeWireSessionIsAvailable()) {
    std::cout << "PipeWire session socket is unavailable; skipping integration test\n";
    return 77;
  }

  const auto restored =
      pipetune::restorePipeWireDefaultSink("pipetune_restore_test");
  return check(restored.success, restored.error) &&
                 check(!restored.selectedTarget.empty(),
                       "default restoration must select a physical sink") &&
                 check(restored.selectedTarget != "pipetune_restore_test",
                       "default restoration must not select the excluded sink")
             ? 0
             : 1;
}
