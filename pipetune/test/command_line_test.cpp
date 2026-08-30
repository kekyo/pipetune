/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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
         check(result.options.ratePolicy.mode ==
                   pipetune::SampleRateMode::automatic &&
                   result.options.ratePolicy.fixedRate == 0 &&
                   result.options.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "direct-run rate policy must default to automatic") &&
         check(result.options.dspBackend ==
                   pipetune::DspBackendKind::scalar,
               "direct-run DSP backend must default to scalar") &&
         check(result.options.dspSimdVariant ==
                   pipetune::DspSimdVariant::automatic,
               "direct-run SIMD variant must default to automatic") &&
         check(result.options.channelCount == 2, "default channels differ") &&
         check(!result.options.checkOnly, "normal run must not stop after readiness");
}

static bool testExplicitOptions() {
  constexpr auto arguments = std::array<std::string_view, 11>{
      "--check", "--channels", "16", "--socket", "/tmp/pipetune.sock",
      "--dsp-backend", "simd", "--dsp-variant", "x86-64-v4",
      "--preset", "studio.effetune_preset"};
  const auto result = pipetune::parseCommandLine(arguments);
  return check(result.error.empty(), result.error) &&
         check(result.options.checkOnly, "--check must select readiness mode") &&
         check(result.options.channelCount == 16, "explicit channels differ") &&
         check(result.options.controlSocketPath == "/tmp/pipetune.sock",
               "explicit control socket differs") &&
         check(result.options.dspBackend ==
                   pipetune::DspBackendKind::simd,
               "explicit direct-run DSP backend differs") &&
         check(result.options.dspSimdVariant ==
                   pipetune::DspSimdVariant::x86_64_v4,
               "explicit direct-run SIMD variant differs");
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

static bool testDaemonAction() {
  constexpr auto defaults = std::array<std::string_view, 1>{"daemon"};
  constexpr auto configured = std::array<std::string_view, 3>{
      "daemon", "--config", "/tmp/pipetune/environment"};
  const auto defaultResult = pipetune::parseCommandLine(defaults);
  const auto configuredResult = pipetune::parseCommandLine(configured);
  return check(defaultResult.error.empty(), defaultResult.error) &&
         check(defaultResult.options.action ==
                   pipetune::CommandLineAction::daemon,
               "daemon subcommand must select daemon mode") &&
         check(defaultResult.options.configPath.empty(),
               "daemon configuration path must default to XDG resolution") &&
         check(configuredResult.error.empty(), configuredResult.error) &&
         check(configuredResult.options.action ==
                   pipetune::CommandLineAction::daemon,
               "configured daemon action differs") &&
         check(configuredResult.options.configPath ==
                   "/tmp/pipetune/environment",
               "explicit daemon configuration path differs");
}

static bool testBypassAction() {
  constexpr auto defaults = std::array<std::string_view, 1>{"bypass"};
  constexpr auto explicitSocket = std::array<std::string_view, 3>{
      "bypass", "--socket", "/tmp/pipetune.sock"};
  const auto defaultResult = pipetune::parseCommandLine(defaults);
  const auto explicitResult = pipetune::parseCommandLine(explicitSocket);
  return check(defaultResult.error.empty(), defaultResult.error) &&
         check(defaultResult.options.action ==
                   pipetune::CommandLineAction::bypass,
               "bypass subcommand must select persistent bypass") &&
         check(defaultResult.options.controlSocketPath.empty(),
               "bypass socket must default to XDG resolution") &&
         check(explicitResult.error.empty(), explicitResult.error) &&
         check(explicitResult.options.action ==
                   pipetune::CommandLineAction::bypass,
               "explicit bypass action differs") &&
         check(explicitResult.options.controlSocketPath ==
                   "/tmp/pipetune.sock",
               "explicit bypass socket differs");
}

static bool testRateActions() {
  constexpr auto list =
      std::array<std::string_view, 2>{"rate", "list"};
  constexpr auto jsonList = std::array<std::string_view, 5>{
      "rate", "list", "--json", "--socket", "/tmp/pipetune.sock"};
  constexpr auto get =
      std::array<std::string_view, 3>{"rate", "get", "--json"};
  constexpr auto setAutomatic = std::array<std::string_view, 5>{
      "rate", "set", "automatic", "--socket", "/tmp/pipetune.sock"};
  constexpr auto setFixed =
      std::array<std::string_view, 4>{"rate", "set", "384000", "force"};

  const auto listResult = pipetune::parseCommandLine(list);
  const auto jsonListResult = pipetune::parseCommandLine(jsonList);
  const auto getResult = pipetune::parseCommandLine(get);
  const auto automaticResult = pipetune::parseCommandLine(setAutomatic);
  const auto fixedResult = pipetune::parseCommandLine(setFixed);
  return check(listResult.error.empty(), listResult.error) &&
         check(listResult.options.action ==
                   pipetune::CommandLineAction::rateList &&
                   !listResult.options.json,
               "rate list action differs") &&
         check(jsonListResult.error.empty(), jsonListResult.error) &&
         check(jsonListResult.options.action ==
                   pipetune::CommandLineAction::rateList &&
                   jsonListResult.options.json &&
                   jsonListResult.options.controlSocketPath ==
                       "/tmp/pipetune.sock",
               "rate list --json action differs") &&
         check(getResult.error.empty(), getResult.error) &&
         check(getResult.options.action ==
                   pipetune::CommandLineAction::rateGet &&
                   getResult.options.json,
               "rate get action differs") &&
         check(automaticResult.error.empty(), automaticResult.error) &&
         check(automaticResult.options.action ==
                   pipetune::CommandLineAction::rateSet &&
                   automaticResult.options.ratePolicy.mode ==
                       pipetune::SampleRateMode::automatic &&
                   automaticResult.options.ratePolicy.fixedRate == 0 &&
                   automaticResult.options.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "rate set automatic differs") &&
         check(automaticResult.options.controlSocketPath ==
                   "/tmp/pipetune.sock",
               "rate set socket differs") &&
         check(fixedResult.error.empty(), fixedResult.error) &&
         check(fixedResult.options.action ==
                   pipetune::CommandLineAction::rateSet &&
                   fixedResult.options.ratePolicy.mode ==
                       pipetune::SampleRateMode::fixed &&
                   fixedResult.options.ratePolicy.fixedRate == 384000 &&
                   fixedResult.options.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::force,
               "rate set fixed force differs");
}

static bool testDspActions() {
  constexpr auto list =
      std::array<std::string_view, 2>{"dsp", "list"};
  constexpr auto get = std::array<std::string_view, 5>{
      "dsp", "get", "--json", "--socket", "/tmp/pipetune.sock"};
  constexpr auto setScalar =
      std::array<std::string_view, 3>{"dsp", "set", "scalar"};
  constexpr auto setSimd = std::array<std::string_view, 7>{
      "dsp", "set", "simd", "--variant", "x86-64-v3",
      "--socket", "/tmp/pipetune.sock"};
  const auto listResult = pipetune::parseCommandLine(list);
  const auto getResult = pipetune::parseCommandLine(get);
  const auto scalarResult = pipetune::parseCommandLine(setScalar);
  const auto simdResult = pipetune::parseCommandLine(setSimd);
  return check(listResult.error.empty(), listResult.error) &&
         check(listResult.options.action ==
                       pipetune::CommandLineAction::dspList &&
                   !listResult.options.json,
               "dsp list action differs") &&
         check(getResult.error.empty(), getResult.error) &&
         check(getResult.options.action ==
                       pipetune::CommandLineAction::dspGet &&
                   getResult.options.json &&
                   getResult.options.controlSocketPath ==
                       "/tmp/pipetune.sock",
               "dsp get action differs") &&
         check(scalarResult.error.empty(), scalarResult.error) &&
         check(scalarResult.options.action ==
                       pipetune::CommandLineAction::dspSet &&
                   scalarResult.options.dspBackend ==
                       pipetune::DspBackendKind::scalar,
               "dsp set scalar action differs") &&
         check(simdResult.error.empty(), simdResult.error) &&
         check(simdResult.options.action ==
                       pipetune::CommandLineAction::dspSet &&
                   simdResult.options.dspBackend ==
                       pipetune::DspBackendKind::simd &&
                   simdResult.options.dspSimdVariant ==
                       pipetune::DspSimdVariant::x86_64_v3,
               "dsp set SIMD action differs");
}

static bool testUserSetupActions() {
  constexpr auto setup = std::array<std::string_view, 1>{"setup"};
  constexpr auto forcedSetup =
      std::array<std::string_view, 2>{"setup", "--force"};
  constexpr auto shortForcedSetup =
      std::array<std::string_view, 2>{"setup", "-f"};
  constexpr auto setupWithoutGtk =
      std::array<std::string_view, 2>{"setup", "--no-launch-gtk"};
  constexpr auto setupPreset = std::array<std::string_view, 3>{
      "setup", "--preset", "/tmp/setup.effetune_preset"};
  constexpr auto unsetup = std::array<std::string_view, 1>{"unsetup"};
  constexpr auto purge =
      std::array<std::string_view, 2>{"unsetup", "--purge"};
  const auto setupResult = pipetune::parseCommandLine(setup);
  const auto forcedSetupResult = pipetune::parseCommandLine(forcedSetup);
  const auto shortForcedSetupResult =
      pipetune::parseCommandLine(shortForcedSetup);
  const auto setupWithoutGtkResult =
      pipetune::parseCommandLine(setupWithoutGtk);
  const auto presetResult = pipetune::parseCommandLine(setupPreset);
  const auto unsetupResult = pipetune::parseCommandLine(unsetup);
  const auto purgeResult = pipetune::parseCommandLine(purge);
  return check(setupResult.error.empty(), setupResult.error) &&
         check(setupResult.options.action ==
                   pipetune::CommandLineAction::setup &&
                   setupResult.options.presetPath.empty() &&
                   !setupResult.options.forceSetup &&
                   setupResult.options.launchGtk,
               "setup without a preset must preserve startup selection") &&
         check(forcedSetupResult.error.empty(), forcedSetupResult.error) &&
         check(forcedSetupResult.options.forceSetup &&
                   forcedSetupResult.options.launchGtk,
               "setup --force must request unconditional setup") &&
         check(shortForcedSetupResult.error.empty(),
               shortForcedSetupResult.error) &&
         check(shortForcedSetupResult.options.forceSetup,
               "setup -f must alias --force") &&
         check(setupWithoutGtkResult.error.empty(),
               setupWithoutGtkResult.error) &&
         check(!setupWithoutGtkResult.options.forceSetup &&
                   !setupWithoutGtkResult.options.launchGtk,
               "GTK bootstrap setup must suppress a recursive GTK launch") &&
         check(presetResult.error.empty(), presetResult.error) &&
         check(presetResult.options.action ==
                   pipetune::CommandLineAction::setup &&
                   presetResult.options.presetPath ==
                       "/tmp/setup.effetune_preset",
               "setup preset selection differs") &&
         check(unsetupResult.error.empty(), unsetupResult.error) &&
         check(unsetupResult.options.action ==
                   pipetune::CommandLineAction::unsetup &&
                   !unsetupResult.options.purge,
               "unsetup defaults differ") &&
         check(purgeResult.error.empty(), purgeResult.error) &&
         check(purgeResult.options.action ==
                   pipetune::CommandLineAction::unsetup &&
                   purgeResult.options.purge,
               "unsetup --purge selection differs");
}

static bool testConfigResetAction() {
  constexpr auto interactive =
      std::array<std::string_view, 2>{"config", "reset"};
  constexpr auto shortYes =
      std::array<std::string_view, 3>{"config", "reset", "-y"};
  constexpr auto longYes =
      std::array<std::string_view, 3>{"config", "reset", "--yes"};
  const auto interactiveResult = pipetune::parseCommandLine(interactive);
  const auto shortYesResult = pipetune::parseCommandLine(shortYes);
  const auto longYesResult = pipetune::parseCommandLine(longYes);
  return check(interactiveResult.error.empty(), interactiveResult.error) &&
         check(interactiveResult.options.action ==
                   pipetune::CommandLineAction::configReset &&
                   !interactiveResult.options.assumeYes,
               "config reset must require confirmation by default") &&
         check(shortYesResult.error.empty(), shortYesResult.error) &&
         check(shortYesResult.options.action ==
                   pipetune::CommandLineAction::configReset &&
                   shortYesResult.options.assumeYes,
               "config reset -y must skip confirmation") &&
         check(longYesResult.error.empty(), longYesResult.error) &&
         check(longYesResult.options.action ==
                   pipetune::CommandLineAction::configReset &&
                   longYesResult.options.assumeYes,
               "config reset --yes must skip confirmation");
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
         check(pipetune::commandLineUsage().find(
                   "pipetune daemon [--config PATH]") !=
                   std::string_view::npos,
               "usage must explain daemon startup") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune bypass [--socket PATH]") !=
                   std::string_view::npos,
               "usage must explain persistent bypass") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune rate get [--json] [--socket PATH]") !=
                   std::string_view::npos,
               "usage must explain rate status") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune rate list [--json] [--socket PATH]") !=
                   std::string_view::npos,
               "usage must explain rate capability listing") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune rate set RATE ENFORCEMENT [--socket PATH]") !=
                   std::string_view::npos,
               "usage must explain rate selection") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune dsp set scalar|simd [--variant VARIANT] "
                   "[--socket PATH]") !=
                   std::string_view::npos,
               "usage must explain DSP backend selection") &&
         check(pipetune::commandLineUsage().find("--rate HZ") ==
                   std::string_view::npos,
               "usage must not advertise legacy direct --rate") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune setup [-f|--force] [--no-launch-gtk] "
                   "[--preset FILE]") !=
                   std::string_view::npos,
               "usage must explain per-user setup") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune unsetup [--purge]") !=
                   std::string_view::npos,
               "usage must explain per-user unsetup") &&
         check(pipetune::commandLineUsage().find(
                   "pipetune config reset [-y|--yes]") !=
                   std::string_view::npos,
               "usage must explain configuration reset") &&
         check(pipetune::commandLineUsage().find(
                   "1 through 16 planar channels") !=
                   std::string_view::npos,
               "usage must describe the EffeTune 2.7 channel range") &&
         check(pipetune::commandLineUsage().find(
                   "Reset Bypass, PCM rate, and DSP backend.") !=
                   std::string_view::npos,
               "usage must describe every reset selection");
}

static bool testRejectedArguments() {
  constexpr auto missingPreset = std::array<std::string_view, 0>{};
  constexpr auto missingValue = std::array<std::string_view, 1>{"--preset"};
  constexpr auto legacyRate =
      std::array<std::string_view, 4>{"--preset", "x.effetune_preset", "--rate", "31999"};
  constexpr auto badChannels =
      std::array<std::string_view, 4>{"--preset", "x.effetune_preset", "--channels", "17"};
  constexpr auto duplicate = std::array<std::string_view, 4>{
      "--preset", "x.effetune_preset", "--preset", "y.effetune_preset"};
  constexpr auto unknown =
      std::array<std::string_view, 3>{"--preset", "x.effetune_preset", "--future"};
  constexpr auto mixedAction =
      std::array<std::string_view, 3>{"--help", "--preset", "x.effetune_preset"};
  constexpr auto mixedRunAndLoad = std::array<std::string_view, 4>{
      "--preset", "x.effetune_preset", "--load-preset",
      "y.effetune_preset"};
  constexpr auto missingLoadValue =
      std::array<std::string_view, 1>{"--load-preset"};
  constexpr auto duplicateSocket = std::array<std::string_view, 5>{
      "--status", "--socket", "/tmp/a", "--socket", "/tmp/b"};
  constexpr auto daemonWithPreset = std::array<std::string_view, 3>{
      "daemon", "--preset", "x.effetune_preset"};
  constexpr auto missingConfig =
      std::array<std::string_view, 2>{"daemon", "--config"};
  constexpr auto duplicateConfig = std::array<std::string_view, 5>{
      "daemon", "--config", "/tmp/a", "--config", "/tmp/b"};
  constexpr auto topLevelConfig =
      std::array<std::string_view, 2>{"--config", "/tmp/a"};
  constexpr auto bypassWithPreset = std::array<std::string_view, 3>{
      "bypass", "--preset", "x.effetune_preset"};
  constexpr auto duplicateBypassSocket = std::array<std::string_view, 5>{
      "bypass", "--socket", "/tmp/a", "--socket", "/tmp/b"};
  constexpr auto setupWithPurge =
      std::array<std::string_view, 2>{"setup", "--purge"};
  constexpr auto duplicateSetupPreset = std::array<std::string_view, 5>{
      "setup", "--preset", "/tmp/a.effetune_preset", "--preset",
      "/tmp/b.effetune_preset"};
  constexpr auto duplicateSetupForce =
      std::array<std::string_view, 3>{"setup", "-f", "--force"};
  constexpr auto duplicateNoLaunchGtk = std::array<std::string_view, 3>{
      "setup", "--no-launch-gtk", "--no-launch-gtk"};
  constexpr auto unsetupWithPreset = std::array<std::string_view, 3>{
      "unsetup", "--preset", "/tmp/a.effetune_preset"};
  constexpr auto duplicatePurge =
      std::array<std::string_view, 3>{"unsetup", "--purge", "--purge"};
  constexpr auto rateWithoutAction =
      std::array<std::string_view, 1>{"rate"};
  constexpr auto invalidRate =
      std::array<std::string_view, 4>{"rate", "set", "88200", "suggest"};
  constexpr auto invalidEnforcement =
      std::array<std::string_view, 4>{"rate", "set", "96000", "strict"};
  constexpr auto missingRateArgument =
      std::array<std::string_view, 2>{"rate", "set"};
  constexpr auto rateSetWithJson = std::array<std::string_view, 5>{
      "rate", "set", "48000", "force", "--json"};
  constexpr auto duplicateRateSocket = std::array<std::string_view, 6>{
      "rate", "get", "--socket", "/tmp/a", "--socket", "/tmp/b"};
  constexpr auto dspWithoutAction =
      std::array<std::string_view, 1>{"dsp"};
  constexpr auto unknownDspAction =
      std::array<std::string_view, 2>{"dsp", "future"};
  constexpr auto missingDspBackend =
      std::array<std::string_view, 2>{"dsp", "set"};
  constexpr auto invalidDspBackend =
      std::array<std::string_view, 3>{"dsp", "set", "avx2"};
  constexpr auto scalarDspVariant = std::array<std::string_view, 5>{
      "dsp", "set", "scalar", "--variant", "auto"};
  constexpr auto missingDspVariant = std::array<std::string_view, 4>{
      "dsp", "set", "simd", "--variant"};
  constexpr auto invalidDspVariant = std::array<std::string_view, 5>{
      "dsp", "set", "simd", "--variant", "avx2"};
  constexpr auto duplicateDspVariant = std::array<std::string_view, 7>{
      "dsp", "set", "simd", "--variant", "baseline", "--variant", "auto"};
  constexpr auto dspSetWithJson =
      std::array<std::string_view, 4>{"dsp", "set", "simd", "--json"};
  constexpr auto duplicateDspSocket = std::array<std::string_view, 6>{
      "dsp", "get", "--socket", "/tmp/a", "--socket", "/tmp/b"};
  constexpr auto invalidDirectDsp = std::array<std::string_view, 4>{
      "--preset", "x.effetune_preset", "--dsp-backend", "avx2"};
  constexpr auto duplicateDirectDsp = std::array<std::string_view, 6>{
      "--preset", "x.effetune_preset", "--dsp-backend", "scalar",
      "--dsp-backend", "simd"};
  constexpr auto scalarWithVariant = std::array<std::string_view, 6>{
      "--preset", "x.effetune_preset", "--dsp-backend", "scalar",
      "--dsp-variant", "baseline"};
  constexpr auto invalidDirectVariant = std::array<std::string_view, 4>{
      "--preset", "x.effetune_preset", "--dsp-variant", "avx2"};
  constexpr auto configWithoutAction =
      std::array<std::string_view, 1>{"config"};
  constexpr auto unknownConfigAction =
      std::array<std::string_view, 2>{"config", "future"};
  constexpr auto duplicateConfigYes =
      std::array<std::string_view, 4>{"config", "reset", "-y", "--yes"};
  constexpr auto unknownConfigOption =
      std::array<std::string_view, 3>{"config", "reset", "--future"};

  return check(!pipetune::parseCommandLine(missingPreset).error.empty(),
               "missing preset must fail") &&
         check(!pipetune::parseCommandLine(missingValue).error.empty(),
               "missing option value must fail") &&
         check(!pipetune::parseCommandLine(legacyRate).error.empty(),
               "legacy direct --rate must be rejected") &&
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
         check(!pipetune::parseCommandLine(missingLoadValue).error.empty(),
               "live loading requires a preset value") &&
         check(!pipetune::parseCommandLine(duplicateSocket).error.empty(),
               "duplicate socket options must fail") &&
         check(!pipetune::parseCommandLine(daemonWithPreset).error.empty(),
               "daemon must reject legacy run options") &&
         check(!pipetune::parseCommandLine(missingConfig).error.empty(),
               "daemon configuration requires a path") &&
         check(!pipetune::parseCommandLine(duplicateConfig).error.empty(),
               "daemon must reject duplicate configuration paths") &&
         check(!pipetune::parseCommandLine(topLevelConfig).error.empty(),
               "--config must be scoped to the daemon subcommand") &&
         check(!pipetune::parseCommandLine(bypassWithPreset).error.empty(),
               "bypass must reject preset arguments") &&
         check(!pipetune::parseCommandLine(duplicateBypassSocket).error.empty(),
               "bypass must reject duplicate socket paths") &&
         check(!pipetune::parseCommandLine(setupWithPurge).error.empty(),
               "setup must reject --purge") &&
         check(!pipetune::parseCommandLine(duplicateSetupPreset).error.empty(),
               "setup must reject duplicate presets") &&
         check(!pipetune::parseCommandLine(duplicateSetupForce).error.empty(),
               "setup must reject duplicate force options") &&
         check(!pipetune::parseCommandLine(duplicateNoLaunchGtk).error.empty(),
               "setup must reject duplicate GTK launch suppression") &&
         check(!pipetune::parseCommandLine(unsetupWithPreset).error.empty(),
               "unsetup must reject preset selection") &&
         check(!pipetune::parseCommandLine(duplicatePurge).error.empty(),
               "unsetup must reject duplicate --purge") &&
         check(!pipetune::parseCommandLine(rateWithoutAction).error.empty(),
               "rate must require a subcommand") &&
         check(!pipetune::parseCommandLine(invalidRate).error.empty(),
               "rate set must reject unsupported DSP rates") &&
         check(!pipetune::parseCommandLine(invalidEnforcement).error.empty(),
               "rate set must reject unknown enforcement") &&
         check(!pipetune::parseCommandLine(missingRateArgument).error.empty(),
               "rate set must require rate and enforcement") &&
         check(!pipetune::parseCommandLine(rateSetWithJson).error.empty(),
               "rate set must reject --json") &&
         check(!pipetune::parseCommandLine(duplicateRateSocket).error.empty(),
               "rate get must reject duplicate sockets") &&
         check(!pipetune::parseCommandLine(dspWithoutAction).error.empty(),
               "dsp must require a subcommand") &&
         check(!pipetune::parseCommandLine(unknownDspAction).error.empty(),
               "dsp must reject unknown subcommands") &&
         check(!pipetune::parseCommandLine(missingDspBackend).error.empty(),
               "dsp set must require a backend") &&
         check(!pipetune::parseCommandLine(invalidDspBackend).error.empty(),
               "dsp set must reject unknown backends") &&
         check(!pipetune::parseCommandLine(scalarDspVariant).error.empty(),
               "dsp set scalar must reject SIMD variant options") &&
         check(!pipetune::parseCommandLine(missingDspVariant).error.empty(),
               "dsp set must require a SIMD variant value") &&
         check(!pipetune::parseCommandLine(invalidDspVariant).error.empty(),
               "dsp set must reject unknown SIMD variants") &&
         check(!pipetune::parseCommandLine(duplicateDspVariant).error.empty(),
               "dsp set must reject duplicate SIMD variants") &&
         check(!pipetune::parseCommandLine(dspSetWithJson).error.empty(),
               "dsp set must reject --json") &&
         check(!pipetune::parseCommandLine(duplicateDspSocket).error.empty(),
               "dsp get must reject duplicate sockets") &&
         check(!pipetune::parseCommandLine(invalidDirectDsp).error.empty(),
               "direct run must reject unknown DSP backends") &&
         check(!pipetune::parseCommandLine(duplicateDirectDsp).error.empty(),
               "direct run must reject duplicate DSP backends") &&
         check(!pipetune::parseCommandLine(scalarWithVariant).error.empty(),
               "scalar direct run must reject a SIMD variant") &&
         check(!pipetune::parseCommandLine(invalidDirectVariant).error.empty(),
               "direct run must reject unknown SIMD variants") &&
         check(!pipetune::parseCommandLine(configWithoutAction).error.empty(),
               "config must require a subcommand") &&
         check(!pipetune::parseCommandLine(unknownConfigAction).error.empty(),
               "config must reject unknown subcommands") &&
         check(!pipetune::parseCommandLine(duplicateConfigYes).error.empty(),
               "config reset must reject duplicate confirmation options") &&
         check(!pipetune::parseCommandLine(unknownConfigOption).error.empty(),
               "config reset must reject unknown options");
}

int main() {
  const auto passed = testRunDefaults() && testExplicitOptions() &&
                      testControlActions() && testDaemonAction() &&
                      testBypassAction() && testRateActions() &&
                      testDspActions() &&
                      testUserSetupActions() &&
                      testConfigResetAction() &&
                      testInformationalActions() && testRejectedArguments();
  return passed ? 0 : 1;
}
