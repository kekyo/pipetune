/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "active_preset_file_monitor.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <poll.h>
#include <string>
#include <string_view>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool writeFile(const std::filesystem::path &path,
                      std::string_view contents) {
  auto stream = std::ofstream(path, std::ios::binary | std::ios::trunc);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  stream.close();
  return stream.good();
}

static bool replaceFile(const std::filesystem::path &path,
                        std::string_view contents) {
  auto temporary = path;
  temporary += ".replacement";
  if (!writeFile(temporary, contents)) {
    return false;
  }
  auto error = std::error_code{};
  std::filesystem::rename(temporary, path, error);
  return !error;
}

static bool waitForChange(pipetune::ActivePresetFileMonitor &monitor,
                          const std::filesystem::path &expectedPath,
                          std::string_view operation) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(2);
  while (std::chrono::steady_clock::now() < deadline) {
    auto descriptor = pollfd{.fd = monitor.descriptor(),
                             .events = POLLIN,
                             .revents = 0};
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    const auto ready = poll(&descriptor, 1, static_cast<int>(remaining.count()));
    if (ready < 0) {
      return check(false, "active preset monitor polling failed");
    }
    if (ready == 0) {
      break;
    }
    const auto event = monitor.consume();
    if (!check(event.error.empty(), event.error)) {
      return false;
    }
    if (event.changed) {
      return check(event.path == expectedPath,
                   "active preset monitor reported another path");
    }
  }
  return check(false, std::string(operation) + " was not reported");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-active-preset-monitor-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto first = directory / "first.effetune_preset";
  const auto second = directory / "second.effetune_preset";
  if (!writeFile(first, "first") || !writeFile(second, "second")) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto created = pipetune::createActivePresetFileMonitor(first);
  if (!check(created.error.empty(), created.error) ||
      !check(created.monitor != nullptr,
             "active preset monitor was not created")) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto passed =
      check(replaceFile(first, "atomic replacement"),
            "cannot replace monitored preset") &&
      waitForChange(*created.monitor, first,
                    "atomic active preset replacement") &&
      check(writeFile(first, "in-place replacement"),
            "cannot rewrite monitored preset") &&
      waitForChange(*created.monitor, first,
                    "in-place active preset replacement");
  if (passed) {
    auto error = std::error_code{};
    std::filesystem::remove(first, error);
    passed = check(!error, "cannot delete monitored preset") &&
             waitForChange(*created.monitor, first,
                           "active preset deletion") &&
             check(writeFile(first, "recreated"),
                   "cannot recreate monitored preset") &&
             waitForChange(*created.monitor, first,
                           "active preset recreation");
  }
  if (passed) {
    const auto error = created.monitor->setPath(second);
    passed = check(error.empty(), error) &&
             check(writeFile(second, "second updated"),
                   "cannot rewrite replacement monitored preset") &&
             waitForChange(*created.monitor, second,
                           "replacement active preset update");
  }

  created.monitor.reset();
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
