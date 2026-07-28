#include "preset-catalog.h"

#include "pipetune/dsp_pipeline.h"

#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>

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
      testBundledStandardPresets(argv[1]);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
