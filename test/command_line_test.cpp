#include "command_line.h"

#include <array>
#include <iostream>
#include <span>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testRunDefaults() {
  constexpr auto arguments =
      std::array<std::string_view, 2>{"--preset", "music.effetune_preset"};
  const auto result = pipetune::parseCommandLine(arguments);
  return check(result.error.empty(), result.error) &&
         check(result.options.action == pipetune::CommandLineAction::run,
               "preset arguments must select run mode") &&
         check(result.options.presetPath == "music.effetune_preset",
               "preset path differs") &&
         check(result.options.targetObject.empty(), "default target must be automatic") &&
         check(result.options.sinkName == "pipetune_sink", "default sink name differs") &&
         check(result.options.sampleRate == 48000, "default rate differs") &&
         check(result.options.channelCount == 2, "default channels differ") &&
         check(!result.options.checkOnly, "normal run must not stop after readiness");
}

static bool testExplicitOptions() {
  constexpr auto arguments = std::array<std::string_view, 11>{
      "--check",   "--channels", "8",          "--target",
      "alsa_out", "--rate",     "192000",     "--sink-name",
      "studio",   "--preset",   "studio.effetune_preset"};
  const auto result = pipetune::parseCommandLine(arguments);
  return check(result.error.empty(), result.error) &&
         check(result.options.checkOnly, "--check must select readiness mode") &&
         check(result.options.channelCount == 8, "explicit channels differ") &&
         check(result.options.sampleRate == 192000, "explicit rate differs") &&
         check(result.options.targetObject == "alsa_out", "explicit target differs") &&
         check(result.options.sinkName == "studio", "explicit sink name differs");
}

static bool testInformationalActions() {
  constexpr auto help = std::array<std::string_view, 1>{"--help"};
  constexpr auto version = std::array<std::string_view, 1>{"--version"};
  const auto helpResult = pipetune::parseCommandLine(help);
  const auto versionResult = pipetune::parseCommandLine(version);
  return check(helpResult.error.empty() &&
                   helpResult.options.action == pipetune::CommandLineAction::help,
               "--help must select help") &&
         check(versionResult.error.empty() &&
                   versionResult.options.action == pipetune::CommandLineAction::version,
               "--version must select version") &&
         check(pipetune::commandLineUsage().find("--preset FILE") !=
                   std::string_view::npos,
               "usage must explain the required preset");
}

static bool testRejectedArguments() {
  constexpr auto missingPreset = std::array<std::string_view, 0>{};
  constexpr auto missingValue = std::array<std::string_view, 1>{"--preset"};
  constexpr auto badRate =
      std::array<std::string_view, 4>{"--preset", "x.effetune_preset", "--rate", "31999"};
  constexpr auto badChannels =
      std::array<std::string_view, 4>{"--preset", "x.effetune_preset", "--channels", "9"};
  constexpr auto duplicate = std::array<std::string_view, 4>{
      "--preset", "x.effetune_preset", "--preset", "y.effetune_preset"};
  constexpr auto unknown =
      std::array<std::string_view, 3>{"--preset", "x.effetune_preset", "--future"};
  constexpr auto mixedAction =
      std::array<std::string_view, 3>{"--help", "--preset", "x.effetune_preset"};

  return check(!pipetune::parseCommandLine(missingPreset).error.empty(),
               "missing preset must fail") &&
         check(!pipetune::parseCommandLine(missingValue).error.empty(),
               "missing option value must fail") &&
         check(!pipetune::parseCommandLine(badRate).error.empty(),
               "out-of-range rate must fail") &&
         check(!pipetune::parseCommandLine(badChannels).error.empty(),
               "out-of-range channels must fail") &&
         check(!pipetune::parseCommandLine(duplicate).error.empty(),
               "duplicate options must fail") &&
         check(!pipetune::parseCommandLine(unknown).error.empty(),
               "unknown options must fail") &&
         check(!pipetune::parseCommandLine(mixedAction).error.empty(),
               "informational actions must stand alone");
}

int main() {
  const auto passed = testRunDefaults() && testExplicitOptions() &&
                      testInformationalActions() && testRejectedArguments();
  return passed ? 0 : 1;
}
