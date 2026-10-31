/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "measurement_store.h"
#include <pipetune/dsp_pipeline.h>

#include <cmath>
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
  const auto preset = root / "correction.effetune_preset";
  std::ofstream(preset) << R"({"pipeline":[{"name":"Crosstalk Cancellation","parameters":{
    "ll":"ear-l::ch=left","lr":"ear-r::ch=left","rl":"ear-l::ch=right","rr":"ear-r::ch=right","tp":1024,"lt":"128"}}]})";
  const auto context = pipetune::PipelineLoadContext{root, {}};
  auto active = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  if (!active.pipeline) { std::cerr << active.error << '\n'; std::filesystem::remove_all(root); return 1; }
  const auto render = [&] {
    check(active.pipeline->reset() == pipetune::ProcessStatus::ok, "the retained correction must reset");
    auto result = std::vector<float>{};
    for (auto block = 0; block < 64; ++block) {
      auto audio = std::vector<float>(256);
      if (block == 0) audio[0] = 1;
      check(active.pipeline->process(audio, 2, 128, block * 128.0 / 48000) == pipetune::ProcessStatus::ok,
          "the retained correction must process PCM");
      result.insert(result.end(), audio.begin(), audio.end());
    }
    return result;
  };
  // Finish the initial coefficient activation before comparing reset renders.
  static_cast<void>(render());
  const auto corrected = render();
  auto energy = 0.0;
  for (const auto sample : corrected) { check(std::isfinite(sample), "corrected PCM must be finite"); energy += sample * sample; }
  check(energy > 0 && corrected.front() == 0, "the reference must contain delayed correction, not bypassed PCM");
  const auto latency = active.pipeline->latencyFrames();
  // The bound applies to the complete backup, including optional analysis data.
  constexpr auto maximumBytes = std::size_t{64} * 1024u * 1024u;
  {
    auto output = std::ofstream(originalFile);
    output << original << std::string(maximumBytes - original.size(), ' ');
  }
  store = {root, {}};
  const auto atLimit = pipetune::loadCrosstalkMeasurements(store, ids);
  check(atLimit.error.empty() && atLimit.sources[0].samples == valid.sources[0].samples,
        "a complete backup at 64 MiB must retain the linear impulse");
  std::ofstream(originalFile, std::ios::app) << ' ';
  store = {root, {}};
  const auto oversized = pipetune::loadCrosstalkMeasurements(store, ids);
  check(oversized.error.find("byte budget") != std::string::npos && oversized.files.size() == 2,
        "a backup above 64 MiB must fail and retain watched dependencies");
  const auto rejected = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  check(!rejected.pipeline && rejected.error.find("byte budget") != std::string::npos,
      "an oversized source must reject the whole correction candidate");
  check(active.pipeline->activePluginCount() == 1 && active.pipeline->latencyFrames() == latency && render() == corrected,
      "rejecting an oversized candidate must preserve the active correction PCM and latency");
  std::ofstream(originalFile) << original;
  auto recovered = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  check(recovered.pipeline && recovered.warnings.empty(), "a repaired backup must prepare a new correction");
  if (recovered.pipeline) {
    active = std::move(recovered);
    static_cast<void>(render());
    check(render() == corrected, "the repaired correction must recover the same PCM");
  }
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
