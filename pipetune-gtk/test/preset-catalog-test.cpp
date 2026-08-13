/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "preset-catalog.h"
#include "preset-file-monitor.h"

#include "pipetune/dsp_pipeline.h"

#include <glib.h>

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static void writeFile(const std::filesystem::path &path,
                      std::string_view contents) {
  std::filesystem::create_directories(path.parent_path());
  auto stream = std::ofstream(path, std::ios::binary);
  stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

static bool approximately(float actual, float expected) {
  return std::abs(actual - expected) <= 1.0e-6F;
}

static void replaceFile(const std::filesystem::path &path,
                        std::string_view contents) {
  const auto temporaryPath = path.string() + ".new";
  writeFile(temporaryPath, contents);
  std::filesystem::rename(temporaryPath, path);
}

struct MonitorTestState {
  std::filesystem::path userFile;
  std::vector<pipetune_gtk::PresetChoice> choices;
  GMainLoop *loop;
  std::size_t notificationCount;
  bool timedOut;
  bool lastRefreshParsed;
  std::vector<std::string> lastDiagnostics;
};

static void onPresetFileChanged(void *userData) {
  auto *state = static_cast<MonitorTestState *>(userData);
  const auto refresh =
      pipetune_gtk::loadEffeTuneSavedPresets(state->userFile);
  state->choices = pipetune_gtk::applyEffeTuneSavedPresetRefresh(
      state->choices, refresh);
  state->lastRefreshParsed = refresh.parsed;
  state->lastDiagnostics = refresh.diagnostics;
  ++state->notificationCount;
  if (state->loop != nullptr) {
    g_main_loop_quit(state->loop);
  }
}

static gboolean onMonitorWaitTimeout(gpointer userData) {
  auto *state = static_cast<MonitorTestState *>(userData);
  state->timedOut = true;
  if (state->loop != nullptr) {
    g_main_loop_quit(state->loop);
  }
  return G_SOURCE_REMOVE;
}

static bool waitForMonitorNotification(
    MonitorTestState &state, std::size_t previousNotificationCount,
    std::string_view operation) {
  state.timedOut = false;
  state.loop = g_main_loop_new(nullptr, FALSE);
  const auto timeoutSource =
      g_timeout_add_seconds(5, onMonitorWaitTimeout, &state);
  g_main_loop_run(state.loop);
  if (!state.timedOut) {
    g_source_remove(timeoutSource);
  }
  g_main_loop_unref(state.loop);
  state.loop = nullptr;
  return check(!state.timedOut,
               std::string(operation) +
                   " did not produce a file monitor notification") &&
         check(state.notificationCount > previousNotificationCount,
               std::string(operation) +
                   " did not refresh the saved preset list");
}

static bool hasChoice(
    const std::vector<pipetune_gtk::PresetChoice> &choices,
    pipetune_gtk::PresetSource source, std::string_view name) {
  for (const auto &choice : choices) {
    if (choice.source == source && choice.name == name) {
      return true;
    }
  }
  return false;
}

static bool testStoragePathResolution(
    const std::filesystem::path &directory) {
  const auto xdgRoot = directory / "xdg";
  const auto home = directory / "home";
  const auto xdg = pipetune_gtk::resolveEffeTuneUserPresetPath(
      xdgRoot.string(), home);
  const auto fallback =
      pipetune_gtk::resolveEffeTuneUserPresetPath({}, home);
  const auto missing =
      pipetune_gtk::resolveEffeTuneUserPresetPath({}, {});
  return check(xdg.error.empty(), xdg.error) &&
         check(xdg.path ==
                   xdgRoot / "effetune" / "effetune_presets.json",
               "EffeTune XDG preset path differs") &&
         check(fallback.error.empty(), fallback.error) &&
         check(fallback.path ==
                   home / ".config" / "effetune" /
                       "effetune_presets.json",
               "EffeTune HOME preset path differs") &&
         check(!missing.error.empty(),
               "missing EffeTune config roots must be rejected");
}

static bool testCatalogAndMaterialization(
    const std::filesystem::path &directory) {
  const auto systemDirectory = directory / "system";
  const auto userFile =
      directory / "xdg" / "effetune" / "effetune_presets.json";
  const auto snapshotDirectory = directory / "snapshots";
  writeFile(
      systemDirectory / "presets.txt",
      R"txt([categories]
Processor: Processing chains
Utils: Utilities

[presets]
processor/quiet: Quiet | Processor | Reduces volume
 utils/invert : Invert | Utils | Inverts polarity
)txt");
  writeFile(
      systemDirectory / "processor" / "quiet.effetune_preset",
      R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}]})json");
  writeFile(
      systemDirectory / "utils" / "invert.effetune_preset",
      R"json({"plugins":[{"nm":"Polarity Inversion","en":true,"ch":"A"}]})json");
  writeFile(
      userFile,
      R"json({
        "Loud": {
          "plugins": [{"nm":"Volume","en":true,"vl":6,"ch":"A"}]
        },
        "Invert / ../../ unsafe": {
          "plugins": [{"nm":"Polarity Inversion","en":true,"ch":"A"}]
        },
        "Broken": {"metadata": 1}
      })json");

  const auto catalog =
      pipetune_gtk::loadEffeTunePresetCatalog(systemDirectory, userFile);
  if (!check(catalog.choices.size() == 4,
             "catalog must contain two standard and two saved presets") ||
      !check(catalog.diagnostics.size() == 1,
             "invalid saved presets must produce one diagnostic")) {
    return false;
  }
  const auto &quiet = catalog.choices[0];
  const auto &invert = catalog.choices[1];
  const auto &unsafe = catalog.choices[2];
  const auto &loud = catalog.choices[3];
  if (!check(quiet.source == pipetune_gtk::PresetSource::standard &&
                 quiet.name == "Quiet" && quiet.category == "Processor" &&
                 quiet.path ==
                     systemDirectory / "processor" /
                         "quiet.effetune_preset",
             "first standard preset differs") ||
      !check(invert.source == pipetune_gtk::PresetSource::standard &&
                 invert.name == "Invert" && invert.category == "Utils",
             "second standard preset differs") ||
      !check(unsafe.source == pipetune_gtk::PresetSource::saved &&
                 unsafe.name == "Invert / ../../ unsafe",
             "saved presets must be sorted by name") ||
      !check(loud.source == pipetune_gtk::PresetSource::saved &&
                 loud.name == "Loud" && !loud.serializedPreset.empty(),
             "saved preset payload differs")) {
    return false;
  }

  const auto standard =
      pipetune_gtk::resolvePresetChoicePath(quiet, snapshotDirectory);
  const auto materialized =
      pipetune_gtk::resolvePresetChoicePath(loud, snapshotDirectory);
  if (!check(standard.error.empty(), standard.error) ||
      !check(standard.path == quiet.path,
             "standard preset path must be used directly") ||
      !check(materialized.error.empty(), materialized.error) ||
      !check(materialized.path.parent_path() == snapshotDirectory,
             "saved preset snapshot escaped its directory") ||
      !check(materialized.path.extension() == ".effetune_preset",
             "saved preset snapshot extension differs")) {
    return false;
  }

  struct stat metadata {};
  if (!check(stat(materialized.path.c_str(), &metadata) == 0,
             "saved preset snapshot is unavailable") ||
      !check((metadata.st_mode & 0777) == 0600,
             "saved preset snapshot must be private")) {
    return false;
  }

  const auto loaded = pipetune::loadDspPipeline(
      materialized.path,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(loaded.pipeline != nullptr, loaded.error)) {
    return false;
  }
  auto samples = std::vector<float>{0.25F};
  if (!check(loaded.pipeline->process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "materialized saved preset processing failed") ||
      !check(approximately(
                 samples[0],
                 0.25F * std::pow(10.0F, 6.0F / 20.0F)),
             "materialized saved preset DSP output differs")) {
    return false;
  }

  auto updatedLoud = loud;
  updatedLoud.serializedPreset =
      R"json({"plugins":[{"nm":"Volume","en":true,"vl":-6,"ch":"A"}]})json";
  const auto updated = pipetune_gtk::resolvePresetChoicePath(
      updatedLoud, snapshotDirectory);
  if (!check(updated.error.empty(), updated.error) ||
      !check(updated.path == materialized.path,
             "saved preset updates must replace the stable snapshot")) {
    return false;
  }
  const auto updatedPipeline = pipetune::loadDspPipeline(
      updated.path,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(updatedPipeline.pipeline != nullptr,
             updatedPipeline.error)) {
    return false;
  }
  samples = {0.25F};
  if (!check(updatedPipeline.pipeline->process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "updated saved preset processing failed") ||
      !check(approximately(
                 samples[0],
                 0.25F * std::pow(10.0F, -6.0F / 20.0F)),
             "updated saved preset DSP output differs")) {
    return false;
  }

  const auto unsafeMaterialized =
      pipetune_gtk::resolvePresetChoicePath(unsafe, snapshotDirectory);
  return check(unsafeMaterialized.error.empty(),
               unsafeMaterialized.error) &&
         check(unsafeMaterialized.path.parent_path() == snapshotDirectory,
               "unsafe saved preset name escaped its snapshot directory");
}

static bool testMissingAndMalformedSources(
    const std::filesystem::path &directory) {
  const auto systemDirectory = directory / "empty-system";
  const auto missingUserFile = directory / "missing.json";
  std::filesystem::create_directories(systemDirectory);
  const auto missing = pipetune_gtk::loadEffeTunePresetCatalog(
      systemDirectory, missingUserFile);
  if (!check(missing.choices.empty(),
             "missing sources must not create preset choices") ||
      !check(missing.diagnostics.size() == 1,
             "missing standard presets must produce one diagnostic")) {
    return false;
  }

  const auto malformedUserFile = directory / "malformed.json";
  writeFile(malformedUserFile, R"json({"unfinished":)json");
  const auto malformed = pipetune_gtk::loadEffeTunePresetCatalog(
      systemDirectory, malformedUserFile);
  return check(malformed.choices.empty(),
               "malformed sources must not create preset choices") &&
         check(malformed.diagnostics.size() == 2,
               "malformed user storage must add a diagnostic");
}

static bool testActiveSavedPresetSnapshotRefresh(
    const std::filesystem::path &directory) {
  const auto snapshotDirectory = directory / "active-snapshots";
  auto active = pipetune_gtk::PresetChoice{
      .source = pipetune_gtk::PresetSource::saved,
      .name = "Actively used",
      .category = {},
      .path = {},
      .serializedPreset =
          R"json({"plugins":[{"nm":"Volume","en":true,"vl":6,"ch":"A"}]})json",
  };
  const auto materialized = pipetune_gtk::resolvePresetChoicePath(
      active, snapshotDirectory);
  if (!check(materialized.error.empty(), materialized.error)) {
    return false;
  }

  struct stat before {};
  if (!check(stat(materialized.path.c_str(), &before) == 0,
             "active saved preset snapshot is unavailable")) {
    return false;
  }

  active.serializedPreset =
      R"json({"plugins":[{"nm":"Volume","en":true,"vl":-6,"ch":"A"}]})json";
  const auto refreshed =
      pipetune_gtk::refreshActiveSavedPresetSnapshot(
          {active}, materialized.path, snapshotDirectory);
  if (!check(refreshed.error.empty(), refreshed.error) ||
      !check(refreshed.matched,
             "active saved preset snapshot was not matched") ||
      !check(refreshed.changed,
             "changed active saved preset snapshot was not refreshed")) {
    return false;
  }

  const auto loaded = pipetune::loadDspPipeline(
      materialized.path,
      {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
  if (!check(loaded.pipeline != nullptr, loaded.error)) {
    return false;
  }
  auto samples = std::vector<float>{0.25F};
  if (!check(loaded.pipeline->process(samples, 1, 1, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "refreshed saved preset snapshot processing failed") ||
      !check(approximately(
                 samples[0],
                 0.25F * std::pow(10.0F, -6.0F / 20.0F)),
             "refreshed saved preset snapshot DSP output differs")) {
    return false;
  }

  struct stat afterRefresh {};
  if (!check(stat(materialized.path.c_str(), &afterRefresh) == 0,
             "refreshed saved preset snapshot is unavailable") ||
      !check(afterRefresh.st_ino != before.st_ino,
             "changed saved preset snapshot was not atomically replaced")) {
    return false;
  }
  const auto unchanged =
      pipetune_gtk::refreshActiveSavedPresetSnapshot(
          {active}, materialized.path, snapshotDirectory);
  struct stat afterUnchanged {};
  if (!check(stat(materialized.path.c_str(), &afterUnchanged) == 0,
             "unchanged saved preset snapshot is unavailable") ||
      !check(unchanged.error.empty(), unchanged.error) ||
      !check(unchanged.matched && !unchanged.changed,
             "unchanged active snapshot must not be replaced") ||
      !check(afterUnchanged.st_ino == afterRefresh.st_ino,
             "unchanged active snapshot produced a file update")) {
    return false;
  }

  active.serializedPreset =
      R"json({"plugins":[{"nm":"Volume","en":true,"vl":3,"ch":"A"}]})json";
  const auto unrelated =
      pipetune_gtk::refreshActiveSavedPresetSnapshot(
          {active}, directory / "custom.effetune_preset",
          snapshotDirectory);
  struct stat afterUnrelated {};
  return check(stat(materialized.path.c_str(), &afterUnrelated) == 0,
               "active snapshot disappeared after unrelated refresh") &&
         check(unrelated.error.empty(), unrelated.error) &&
         check(!unrelated.matched && !unrelated.changed,
               "unrelated active path matched a saved preset") &&
         check(afterUnrelated.st_ino == afterRefresh.st_ino,
               "unrelated preset refresh changed the active snapshot");
}

static bool testSavedPresetFileMonitoring(
    const std::filesystem::path &directory) {
  const auto userFile =
      directory / "monitored-xdg" / "effetune" /
      "effetune_presets.json";
  const auto snapshotDirectory =
      directory / "monitored-snapshots";
  auto state = MonitorTestState{
      .userFile = userFile,
      .choices =
          {
              {.source = pipetune_gtk::PresetSource::standard,
               .name = "Standard reference",
               .category = "Reference",
               .path = directory / "standard.effetune_preset",
               .serializedPreset = {}},
          },
      .loop = nullptr,
      .notificationCount = 0,
      .timedOut = false,
      .lastRefreshParsed = false,
      .lastDiagnostics = {},
  };
  const auto created = pipetune_gtk::createEffeTunePresetFileMonitor(
      userFile, onPresetFileChanged, &state);
  if (!check(created.error.empty(), created.error) ||
      !check(created.monitor != nullptr,
             "saved preset file monitor was not created")) {
    return false;
  }

  auto passed = true;
  auto previousNotifications = state.notificationCount;
  replaceFile(
      userFile,
      R"json({"Old":{"plugins":[{"nm":"Volume","en":true,"vl":6,"ch":"A"}]}})json");
  passed =
      waitForMonitorNotification(state, previousNotifications,
                                 "initial saved preset creation") &&
      check(state.lastRefreshParsed,
            "valid saved preset creation must parse") &&
      check(state.choices.size() == 2,
            "valid creation must append one saved preset") &&
      check(hasChoice(state.choices,
                      pipetune_gtk::PresetSource::standard,
                      "Standard reference"),
            "saved preset creation removed a standard preset") &&
      check(hasChoice(state.choices, pipetune_gtk::PresetSource::saved,
                      "Old"),
            "created saved preset is unavailable");

  auto snapshot = pipetune_gtk::PresetChoicePathResult{};
  if (passed) {
    snapshot = pipetune_gtk::resolvePresetChoicePath(
        state.choices[1], snapshotDirectory);
    passed =
        check(snapshot.error.empty(), snapshot.error) &&
        check(std::filesystem::is_regular_file(snapshot.path),
              "loaded saved preset snapshot is unavailable");
  }

  if (passed) {
    previousNotifications = state.notificationCount;
    replaceFile(userFile, R"json({"unfinished":)json");
    passed =
        waitForMonitorNotification(state, previousNotifications,
                                   "malformed saved preset update") &&
        check(!state.lastRefreshParsed,
              "malformed saved preset update must not parse") &&
        check(!state.lastDiagnostics.empty(),
              "malformed update must report a diagnostic") &&
        check(state.choices.size() == 2 &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::standard,
                            "Standard reference") &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::saved, "Old"),
              "malformed update changed existing preset entries");
  }

  if (passed) {
    previousNotifications = state.notificationCount;
    std::filesystem::remove(userFile);
    passed =
        waitForMonitorNotification(state, previousNotifications,
                                   "saved preset deletion") &&
        check(!state.lastRefreshParsed,
              "deleted saved preset file must not parse") &&
        check(state.choices.size() == 2 &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::standard,
                            "Standard reference") &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::saved, "Old"),
              "file deletion changed existing preset entries");
  }

  if (passed) {
    previousNotifications = state.notificationCount;
    replaceFile(
        userFile,
        R"json({"New":{"plugins":[{"nm":"Volume","en":true,"vl":-6,"ch":"A"}]}})json");
    passed =
        waitForMonitorNotification(state, previousNotifications,
                                   "valid saved preset replacement") &&
        check(state.lastRefreshParsed,
              "valid replacement must parse") &&
        check(state.choices.size() == 2 &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::standard,
                            "Standard reference") &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::saved, "New") &&
                  !hasChoice(state.choices,
                             pipetune_gtk::PresetSource::saved, "Old"),
              "valid replacement did not replace only saved entries");
  }

  if (passed) {
    previousNotifications = state.notificationCount;
    replaceFile(userFile, "{}");
    passed =
        waitForMonitorNotification(state, previousNotifications,
                                   "empty saved preset replacement") &&
        check(state.lastRefreshParsed,
              "empty object replacement must parse") &&
        check(state.choices.size() == 1 &&
                  hasChoice(state.choices,
                            pipetune_gtk::PresetSource::standard,
                            "Standard reference"),
              "empty saved preset update removed a standard preset");
  }

  if (passed) {
    const auto loaded = pipetune::loadDspPipeline(
        snapshot.path,
        {.sampleRate = 48000.0F, .maxChannels = 1, .maxFrames = 32});
    if (!check(loaded.pipeline != nullptr, loaded.error)) {
      passed = false;
    } else {
      auto samples = std::vector<float>{0.25F};
      passed =
          check(loaded.pipeline->process(samples, 1, 1, 0.0) ==
                    pipetune::ProcessStatus::ok,
                "existing loaded preset snapshot processing failed") &&
          check(approximately(
                    samples[0],
                    0.25F * std::pow(10.0F, 6.0F / 20.0F)),
                "JSON refresh changed an existing loaded preset snapshot");
    }
  }

  pipetune_gtk::destroyEffeTunePresetFileMonitor(created.monitor);
  return passed;
}

static bool testBundledStandardPresets(
    const std::filesystem::path &standardDirectory) {
  const auto catalog = pipetune_gtk::loadEffeTunePresetCatalog(
      standardDirectory, standardDirectory / "missing-user-presets.json");
  if (!check(catalog.diagnostics.empty(),
             "bundled standard preset catalog has diagnostics") ||
      !check(catalog.choices.size() >= 10,
             "bundled standard preset catalog is unexpectedly small")) {
    return false;
  }
  for (const auto &choice : catalog.choices) {
    if (!check(choice.source == pipetune_gtk::PresetSource::standard,
               "bundled catalog contains a non-standard preset")) {
      return false;
    }
    const auto loaded = pipetune::loadDspPipeline(
        choice.path,
        {.sampleRate = 96000.0F, .maxChannels = 8, .maxFrames = 64});
    if (!check(loaded.pipeline != nullptr,
               "bundled preset \"" + choice.name +
                   "\" cannot be loaded: " + loaded.error)) {
      return false;
    }
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "bundled EffeTune preset directory is required\n";
    return 1;
  }
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-preset-catalog-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto passed =
      testStoragePathResolution(directory) &&
      testCatalogAndMaterialization(directory) &&
      testMissingAndMalformedSources(directory) &&
      testActiveSavedPresetSnapshotRefresh(directory) &&
      testSavedPresetFileMonitoring(directory) &&
      testBundledStandardPresets(argv[1]);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
