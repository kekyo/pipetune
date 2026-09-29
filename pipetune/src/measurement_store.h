/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_MEASUREMENT_STORE_H
#define PIPETUNE_MEASUREMENT_STORE_H

#include <pipetune/effetune_paths.h>
#include <yyjson.h>
#include <array>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace pipetune {

// One cache per pipeline preparation; never retained by the audio callback.
struct MeasurementStore {
  std::filesystem::path directory;
  std::map<std::string, std::shared_ptr<yyjson_doc>> documents;
};

struct MeasuredImpulse {
  std::vector<float> samples;
  double sampleRate = 0;
  std::int64_t trimStartSamples = 0;
  std::int64_t onsetIndex = 0;
  double referenceScale = 1;
};

struct CrosstalkMeasurements {
  // Measurement order follows EffeTune's source slots: LL, LR, RL, RR.
  std::array<MeasuredImpulse, 4> sources;
  std::vector<std::filesystem::path> files;
  std::string error;
};

CrosstalkMeasurements loadCrosstalkMeasurements(
    MeasurementStore &store, const std::array<std::string, 4> &ids);

} // namespace pipetune
#endif
