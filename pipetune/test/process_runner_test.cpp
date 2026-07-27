#include "process_runner.h"

#include <array>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

int main() {
  const auto noArguments = std::array<std::string, 0>{};
  const auto succeeded = pipetune::runProcess(
      "/bin/true", noArguments, pipetune::ProcessWaitMode::wait);
  const auto failed = pipetune::runProcess(
      "/bin/false", noArguments, pipetune::ProcessWaitMode::wait);
  const auto missing = pipetune::runProcess(
      "/definitely/missing/pipetune-test-command", noArguments,
      pipetune::ProcessWaitMode::wait);
  const auto detached = pipetune::runProcess(
      "/bin/true", noArguments, pipetune::ProcessWaitMode::detached);

  return check(succeeded.started && succeeded.exitCode == 0 &&
                   succeeded.error.empty(),
               "successful waited process result differs") &&
                 check(failed.started && failed.exitCode != 0,
                       "nonzero process status was not preserved") &&
                 check(!missing.started && !missing.error.empty(),
                       "missing executable must report spawn failure") &&
                 check(detached.started && detached.exitCode == 0 &&
                           detached.error.empty(),
                       "detached process must report successful launch")
             ? 0
             : 1;
}
