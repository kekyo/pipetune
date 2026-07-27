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
         check(result.options.controlSocketPath.empty(),
               "default control socket path must be automatic") &&
         check(result.options.targetObject.empty(), "default target must be automatic") &&
         check(result.options.sinkName == "pipetune_sink", "default sink name differs") &&
         check(result.options.sampleRate == 48000, "default rate differs") &&
         check(result.options.channelCount == 2, "default channels differ") &&
         check(!result.options.checkOnly, "normal run must not stop after readiness");
}

static bool testExplicitOptions() {
  constexpr auto arguments = std::array<std::string_view, 13>{
      "--check",   "--channels", "8",          "--target",
      "alsa_out", "--rate",     "192000",     "--sink-name",
      "studio",   "--socket",   "/tmp/pipetune.sock", "--preset",
      "studio.effetune_preset"};
  const auto result = pipetune::parseCommandLine(arguments);
  return check(result.error.empty(), result.error) &&
         check(result.options.checkOnly, "--check must select readiness mode") &&
         check(result.options.channelCount == 8, "explicit channels differ") &&
         check(result.options.sampleRate == 192000, "explicit rate differs") &&
         check(result.options.targetObject == "alsa_out", "explicit target differs") &&
         check(result.options.sinkName == "studio", "explicit sink name differs") &&
         check(result.options.controlSocketPath == "/tmp/pipetune.sock",
               "explicit control socket differs");
}

static bool testControlActions() {
  constexpr auto load = std::array<std::string_view, 4>{
      "--load-preset", "live.effetune_preset", "--socket", "/tmp/live.sock"};
  constexpr auto status =
      std::array<std::string_view, 3>{"--status", "--socket", "/tmp/live.sock"};
  const auto loadResult = pipetune::parseCommandLine(load);
  const auto statusResult = pipetune::parseCommandLine(status);
  return check(loadResult.error.empty(), loadResult.error) &&
         check(loadResult.options.action ==
                   pipetune::CommandLineAction::loadPreset,
               "--load-preset must select live loading") &&
         check(loadResult.options.presetPath == "live.effetune_preset",
               "live preset path differs") &&
         check(loadResult.options.controlSocketPath == "/tmp/live.sock",
               "load socket path differs") &&
         check(statusResult.error.empty(), statusResult.error) &&
         check(statusResult.options.action ==
                   pipetune::CommandLineAction::status,
               "--status must select status retrieval") &&
         check(statusResult.options.controlSocketPath == "/tmp/live.sock",
               "status socket path differs");
}

static bool testDefaultRestorationAction() {
  constexpr auto defaultArguments =
      std::array<std::string_view, 1>{"--restore-default"};
  constexpr auto namedArguments = std::array<std::string_view, 3>{
      "--restore-default", "--sink-name", "custom_sink"};
  const auto defaultResult = pipetune::parseCommandLine(defaultArguments);
  const auto namedResult = pipetune::parseCommandLine(namedArguments);
  return check(defaultResult.error.empty(), defaultResult.error) &&
         check(defaultResult.options.action ==
                   pipetune::CommandLineAction::restoreDefault,
               "--restore-default must select fail-open restoration") &&
         check(defaultResult.options.sinkName == "pipetune_sink",
               "restoration default sink name differs") &&
         check(namedResult.error.empty(), namedResult.error) &&
         check(namedResult.options.action ==
                   pipetune::CommandLineAction::restoreDefault,
               "named restoration action differs") &&
         check(namedResult.options.sinkName == "custom_sink",
               "restoration exclusion name differs");
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
  constexpr auto mixedRunAndLoad = std::array<std::string_view, 4>{
      "--preset", "x.effetune_preset", "--load-preset",
      "y.effetune_preset"};
  constexpr auto runOptionForStatus =
      std::array<std::string_view, 3>{"--status", "--target", "speaker"};
  constexpr auto missingLoadValue =
      std::array<std::string_view, 1>{"--load-preset"};
  constexpr auto duplicateSocket = std::array<std::string_view, 5>{
      "--status", "--socket", "/tmp/a", "--socket", "/tmp/b"};
  constexpr auto restoreWithPreset = std::array<std::string_view, 3>{
      "--restore-default", "--preset", "x.effetune_preset"};

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
               "informational actions must stand alone") &&
         check(!pipetune::parseCommandLine(mixedRunAndLoad).error.empty(),
               "run and live-load actions must be exclusive") &&
         check(!pipetune::parseCommandLine(runOptionForStatus).error.empty(),
               "run-only options must fail with status") &&
         check(!pipetune::parseCommandLine(missingLoadValue).error.empty(),
               "live loading requires a preset value") &&
         check(!pipetune::parseCommandLine(duplicateSocket).error.empty(),
               "duplicate socket options must fail") &&
         check(!pipetune::parseCommandLine(restoreWithPreset).error.empty(),
               "restoration must reject preset options");
}

int main() {
  const auto passed = testRunDefaults() && testExplicitOptions() &&
                      testControlActions() && testDefaultRestorationAction() &&
                      testInformationalActions() && testRejectedArguments();
  return passed ? 0 : 1;
}
