/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "process_runner.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>
#include <vector>

extern char **environ;

namespace pipetune {

static ProcessResult processError(std::string operation, int errorNumber) {
  return {.started = false,
          .exitCode = -1,
          .error = std::move(operation) + ": " +
                   std::strerror(errorNumber)};
}

ProcessResult runProcess(const std::filesystem::path &executable,
                         std::span<const std::string> arguments,
                         ProcessWaitMode mode) {
  const auto executableString = executable.string();
  if (executableString.empty() ||
      executableString.find('\0') != std::string::npos) {
    return {.started = false,
            .exitCode = -1,
            .error = "process executable path is invalid"};
  }
  for (const auto &argument : arguments) {
    if (argument.find('\0') != std::string::npos) {
      return {.started = false,
              .exitCode = -1,
              .error = "process argument contains NUL"};
    }
  }

  auto argumentPointers = std::vector<char *>{};
  argumentPointers.reserve(arguments.size() + 2);
  argumentPointers.push_back(const_cast<char *>(executableString.c_str()));
  for (const auto &argument : arguments) {
    argumentPointers.push_back(const_cast<char *>(argument.c_str()));
  }
  argumentPointers.push_back(nullptr);

  auto actions = posix_spawn_file_actions_t{};
  auto attributes = posix_spawnattr_t{};
  const auto actionsStatus = posix_spawn_file_actions_init(&actions);
  if (actionsStatus != 0) {
    return processError("cannot initialize process file actions",
                        actionsStatus);
  }
  const auto attributesStatus = posix_spawnattr_init(&attributes);
  if (attributesStatus != 0) {
    posix_spawn_file_actions_destroy(&actions);
    return processError("cannot initialize process attributes",
                        attributesStatus);
  }

  auto setupStatus = 0;
  if (mode == ProcessWaitMode::detached) {
    setupStatus =
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null",
                                         O_RDONLY, 0);
    if (setupStatus == 0) {
      setupStatus = posix_spawn_file_actions_addopen(
          &actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    }
    if (setupStatus == 0) {
      setupStatus = posix_spawn_file_actions_addopen(
          &actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    }
#ifdef POSIX_SPAWN_SETSID
    if (setupStatus == 0) {
      setupStatus =
          posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETSID);
    }
#endif
  }
  if (setupStatus != 0) {
    posix_spawnattr_destroy(&attributes);
    posix_spawn_file_actions_destroy(&actions);
    return processError("cannot configure spawned process", setupStatus);
  }

  auto processId = pid_t{-1};
  const auto spawnStatus =
      posix_spawn(&processId, executableString.c_str(), &actions, &attributes,
                  argumentPointers.data(), environ);
  posix_spawnattr_destroy(&attributes);
  posix_spawn_file_actions_destroy(&actions);
  if (spawnStatus != 0) {
    return processError("cannot spawn " + executableString, spawnStatus);
  }
  if (mode == ProcessWaitMode::detached) {
    return {.started = true, .exitCode = 0, .error = {}};
  }

  auto waitStatus = int{0};
  auto waited = pid_t{-1};
  do {
    waited = waitpid(processId, &waitStatus, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != processId) {
    return {.started = true,
            .exitCode = -1,
            .error = "cannot wait for " + executableString + ": " +
                     std::strerror(errno)};
  }
  if (WIFEXITED(waitStatus)) {
    return {.started = true,
            .exitCode = WEXITSTATUS(waitStatus),
            .error = {}};
  }
  if (WIFSIGNALED(waitStatus)) {
    return {.started = true,
            .exitCode = 128 + WTERMSIG(waitStatus),
            .error = {}};
  }
  return {.started = true,
          .exitCode = -1,
          .error = "spawned process ended with an unsupported wait status"};
}

} // namespace pipetune
