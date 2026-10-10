/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_MEASUREMENT_STORE_H
#define PIPETUNE_MEASUREMENT_STORE_H

#include "asset_file.h"
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
  std::map<std::string, AssetFileSnapshot> snapshots = {};
};

/** Physical measurement identity and optional virtual output channel. */
struct MeasurementReference {
  std::string id; /**< Physical backup basename, without the JSON extension. */
  std::string channel; /**< Virtual output channel, or empty for a mono measurement. */
};

/**
 * Validates an application measurement reference without touching the filesystem.
 * @param reference Physical ID or ID::ch=channel.
 * @return Safe backup identity and selected virtual channel.
 * @throws std::invalid_argument For invalid IDs or empty virtual channels.
 */
MeasurementReference parseMeasurementReference(std::string_view reference);

/**
 * Reads and validates a bounded measurement JSON through the current GIO dispatcher.
 * @param store Per-preparation document and source-identity cache.
 * @param id Physical measurement ID.
 * @param maximumBytes Source size admitted before allocation.
 * @param cancellation Preparation cancellation.
 * @return Document owned by store, whose root ID matches id.
 * @throws std::runtime_error For unavailable, changed, invalid, or oversized data.
 */
cardio::promise<yyjson_doc *> readMeasurementSessionAsync(MeasurementStore &store,
    const std::string &id, std::size_t maximumBytes, cardio::cancellation cancellation);

/**
 * Extracts finite frequency/magnitude pairs from a physical or virtual measurement.
 * @param root Validated measurement JSON root.
 * @param channel Optional virtual output channel, empty for a physical source.
 * @return Positive hertz/decibel pairs, preserving the stored order.
 * @throws std::invalid_argument For absent or malformed frequency responses.
 */
std::vector<std::pair<double, double>> measurementFrequencyResponse(
    yyjson_val *root, std::string_view channel);

struct MeasuredImpulse {
  std::vector<float> samples;
  double sampleRate = 0;
  std::int64_t trimStartSamples = 0;
  std::int64_t onsetIndex = 0;
  double referenceScale = 1;
};

/** One point-ordered impulse without Crosstalk's timing and ear constraints. */
struct MeasuredPointImpulse {
  std::uint64_t pointId = 0; /**< Original nonnegative point identity. */
  MeasuredImpulse impulse; /**< Validated PCM, rate, onset, and reference scale. */
};

/**
 * Resolves a complete point-ordered impulse set from physical or virtual measurements.
 * @param root Validated physical measurement JSON.
 * @param channel Virtual output channel, or empty for a mono measurement.
 * @return Every point's impulse, or an empty vector if any point has no stored IR.
 * @throws std::invalid_argument If a complete stored set contains invalid metadata or PCM.
 * @remarks Output time-reference restrictions belong to the consuming DSP.
 */
std::vector<MeasuredPointImpulse> measurementImpulseResponses(yyjson_val *root, std::string_view channel);

struct CrosstalkMeasurements {
  // Measurement order follows EffeTune's source slots: LL, LR, RL, RR.
  std::array<MeasuredImpulse, 4> sources;
  std::vector<std::filesystem::path> files;
  std::string error;
};

CrosstalkMeasurements loadCrosstalkMeasurements(
    MeasurementStore &store, const std::array<std::string, 4> &ids);

/**
 * Resolves four Crosstalk sources asynchronously with Crosstalk-specific validation.
 * @param store Per-preparation measurement cache.
 * @param ids LL, LR, RL, RR measurement references.
 * @param cancellation Superseded preparation or shutdown notification.
 * @return Validated impulses or a diagnostic retaining every valid dependency.
 */
cardio::promise<CrosstalkMeasurements> loadCrosstalkMeasurementsAsync(
    MeasurementStore &store, const std::array<std::string, 4> &ids, cardio::cancellation cancellation);

} // namespace pipetune
#endif
