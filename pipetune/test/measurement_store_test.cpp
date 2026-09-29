/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "measurement_store.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <unistd.h>

static int failures = 0;
static void check(bool condition, const char *message) {
  if (!condition) { std::cerr << message << '\n'; ++failures; }
}

static void writeMeasurement(const std::filesystem::path &root,
                             const std::string &id, const std::string &data,
                             const std::string &reference) {
  std::ofstream(root / (id + ".json")) <<
      "{\"id\":\"" + id + "\",\"outputChannels\":[\"left\",\"right\"],"
      "\"points\":[{\"pointId\":0,\"channels\":["
      "{\"channel\":\"left\",\"irId\":0,\"ir\":{\"stored\":true}},"
      "{\"channel\":\"right\",\"irId\":1,\"ir\":{\"stored\":true}}]}],"
      "\"impulseResponses\":["
      "{\"measurementId\":\"" + id + "\",\"pointId\":0,\"channel\":\"left\","
      "\"data\":\"" + data + "\",\"sampleRate\":48000,\"trimStartSamples\":0,"
      "\"onsetIndex\":0,\"refScale\":2,\"outputTimeReference\":\"" + reference + "\"},"
      "{\"measurementId\":\"" + id + "\",\"pointId\":1,\"channel\":\"right\","
      "\"data\":\"AAAAPwAAAAA=\",\"sampleRate\":48000,\"trimStartSamples\":0,"
      "\"onsetIndex\":0,\"outputTimeReference\":\"audio-context\"}]}";
}

int main() {
  const auto root = std::filesystem::temp_directory_path() /
      ("pipetune-measurements-" + std::to_string(getpid()));
  std::filesystem::create_directories(root);
  writeMeasurement(root, "ear-l", "AACAPwAAAAA=", "audio-context");
  writeMeasurement(root, "ear-r", "AACAPwAAAAA=", "file");
  const auto ids = std::array<std::string, 4>{
      "ear-l::ch=left", "ear-r::ch=left", "ear-l::ch=right", "ear-r::ch=right"};
  auto store = pipetune::MeasurementStore{root, {}};
  const auto valid = pipetune::loadCrosstalkMeasurements(store, ids);
  check(valid.error.empty(), valid.error.c_str());
  check(valid.files.size() == 2, "two sessions must produce two dependencies");
  check(valid.sources[0].samples == std::vector<float>({1, 0}), "decode float32 little endian");
  check(valid.sources[2].samples == std::vector<float>({0.5, 0}), "resolve channel by irId");
  check(valid.sources[0].referenceScale == 2, "retain deconvolution reference scale");
  // A preparation reads a session once; a subsequent preparation sees new bytes.
  writeMeasurement(root, "ear-l", "AAAAfwAAAAA=", "audio-context");
  check(pipetune::loadCrosstalkMeasurements(store, ids).error.empty(), "cache within a preparation");
  for (const auto &data : {"!invalid!", "AAAA", "AACAfwAAAAA=", "AADAfwAAAAA="}) {
    writeMeasurement(root, "ear-l", data, "audio-context");
    store = {root, {}};
    check(!pipetune::loadCrosstalkMeasurements(store, ids).error.empty(), "reject malformed or nonfinite samples");
  }
  writeMeasurement(root, "ear-l", "AACAPwAAAAA=", "media-element");
  store = {root, {}};
  check(!pipetune::loadCrosstalkMeasurements(store, ids).error.empty(), "reject unsupported time reference");
  writeMeasurement(root, "ear-l", "AACAPwAAAAA=", "audio-context");
  store = {root, {}};
  auto duplicate = ids; duplicate[2] = duplicate[0];
  check(!pipetune::loadCrosstalkMeasurements(store, duplicate).error.empty(), "reject duplicate paths");
  auto unsafe = ids; unsafe[0] = "../escape::ch=left";
  check(!pipetune::loadCrosstalkMeasurements(store, unsafe).error.empty(), "reject invalid measurement IDs");
  // Validate malformed metadata independently of the sample decoder.
  const auto originalFile = root / "ear-l.json";
  auto input = std::ifstream(originalFile);
  const auto original = std::string(std::istreambuf_iterator<char>(input), {});
  for (const auto &[before, after] : std::array<std::pair<std::string, std::string>, 5>{
           {{"\"sampleRate\":48000", "\"sampleRate\":44100"},
            {"\"onsetIndex\":0", "\"onsetIndex\":99"},
            {"\"stored\":true", "\"stored\":false"},
            {"\"irId\":0", "\"irId\":99"},
            {"\"points\":[", "\"points\":[{},"}}}) {
    auto contents = original;
    contents.replace(contents.find(before), before.size(), after);
    std::ofstream(originalFile) << contents;
    store = {root, {}};
    check(!pipetune::loadCrosstalkMeasurements(store, ids).error.empty(),
          "reject inconsistent rate, onset, missing IR, and multiple points");
  }
  std::filesystem::remove(root / "ear-l.json");
  store = {root, {}};
  const auto missing = pipetune::loadCrosstalkMeasurements(store, ids);
  check(!missing.error.empty() && missing.files.size() == 2, "retain missing dependencies for recovery");
  check(pipetune::resolveEffeTuneDirectory("/xdg", "/home/user") == "/xdg/effetune", "prefer XDG config");
  check(pipetune::resolveEffeTuneDirectory({}, "/home/user") == "/home/user/.config/effetune", "fall back to HOME");
  check(pipetune::resolveEffeTuneDirectory({}, {}).empty(), "missing environment has no implicit cwd fallback");
  std::filesystem::remove_all(root);
  return failures == 0 ? 0 : 1;
}
