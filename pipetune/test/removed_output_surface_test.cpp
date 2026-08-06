#include "command_line.h"

#include <array>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool commandIsRejected(
    std::span<const std::string_view> arguments) {
  return !pipetune::parseCommandLine(arguments).error.empty();
}

int main() {
  const auto output =
      std::array<std::string_view, 2>{"output", "list"};
  const auto restore =
      std::array<std::string_view, 1>{"restore-default"};
  const auto target = std::array<std::string_view, 3>{
      "preset.effetune_preset", "--target", "alsa_output.test"};
  const auto usage = pipetune::commandLineUsage();

  return check(commandIsRejected(output),
               "output commands must no longer be accepted") &&
                 check(commandIsRejected(restore),
                       "default-sink restoration must no longer be accepted") &&
                 check(commandIsRejected(target),
                       "direct physical targets must no longer be accepted") &&
                 check(usage.find("output") == std::string_view::npos &&
                           usage.find("restore-default") ==
                               std::string_view::npos &&
                           usage.find("--target") == std::string_view::npos,
                       "removed output operations must be absent from usage")
             ? 0
             : 1;
}
