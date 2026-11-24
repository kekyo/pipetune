/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SPEAKER_CORRECTION_ASSET_H
#define PIPETUNE_SPEAKER_CORRECTION_ASSET_H

#include "asset_file.h"
#include "asset_preparation_resources.h"
#include <yyjson.h>

namespace pipetune {

/** Playback data and source identity of a Distortion Correction measurement. */
struct SpeakerCorrectionLoadResult {
  PreparedDspAsset asset; /**< Ready-to-stage weights or a preparation diagnostic. */
  std::filesystem::path file; /**< Assigned backup to monitor, including after a failed read. */
  std::optional<AssetFileSnapshot> snapshot; /**< Identity to verify before publication. */
  std::optional<float> checkedVolume; /**< Amplifier change from a newer measurement check. */
};

/**
 * Loads the fitted even- and odd-order corrections saved by the measurement app.
 * @param directory EffeTune measurement-backups directory.
 * @param parameters Serialized Distortion Correction settings.
 * @param outputChannels Physical channel count before the pipeline packs selected channels.
 * @param memory Reservation retained by the caller until the native asset has been staged.
 * @param maximumMemoryBytes Combined asset preparation budget.
 * @param cancellation Superseded pipeline or shutdown signal.
 * @return Selected model and dependency, or dry playback when no model is assigned.
 */
cardio::promise<SpeakerCorrectionLoadResult> loadSpeakerCorrectionAsync(
    const std::filesystem::path &directory, yyjson_val *parameters, std::uint32_t outputChannels,
    AssetMemoryReservation &memory, std::uint64_t maximumMemoryBytes, cardio::cancellation cancellation);

} // namespace pipetune
#endif
