#ifndef PIPETUNE_PROCESS_RUNNER_H
#define PIPETUNE_PROCESS_RUNNER_H

#include <filesystem>
#include <span>
#include <string>

namespace pipetune {

/**
 * Selects whether a spawned process is awaited.
 */
enum class ProcessWaitMode {
  /** Wait for process termination and return its status. */
  wait,
  /** Return after a successful spawn with standard streams detached. */
  detached
};

/**
 * Reports process creation and, when awaited, termination.
 */
struct ProcessResult {
  /** True after the executable was successfully spawned. */
  bool started;
  /** Exit status, or 128 plus signal number; zero for a detached launch. */
  int exitCode;
  /** Spawn or wait diagnostic, or empty after a completed operation. */
  std::string error;
};

/**
 * Runs an executable directly without invoking a shell.
 *
 * @param executable Absolute executable path.
 * @param arguments Argument tokens excluding argv[0].
 * @param mode Whether to wait for process termination.
 * @return Spawn and termination result.
 */
ProcessResult runProcess(const std::filesystem::path &executable,
                         std::span<const std::string> arguments,
                         ProcessWaitMode mode);

} // namespace pipetune

#endif
