/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_IR_ASSET_LOADER_H
#define PIPETUNE_IR_ASSET_LOADER_H

#include "asset_preparation_resources.h"
#include "asset_file.h"
#include <pipetune/asset_cache.h>
#include <yyjson.h>
#include <cardio.h>
#include <filesystem>
#include <memory>

namespace pipetune {

struct AssetCachePin;

/** Result of resolving and preparing one registered IR. */
struct IrAssetLoadResult {
  std::shared_ptr<const SharedPreparedAsset> prepared; /**< Shared immutable payload and diagnostics. */
  std::vector<std::filesystem::path> files; /**< All source dependencies, including missing inputs. */
  std::vector<AssetFileSnapshot> snapshots; /**< Inputs to revalidate before pipeline publication. */
  std::string error; /**< Fatal preparation error, or empty. */
  bool cacheHit = false; /**< Whether decoding and coefficient preparation were reused. */
};

/**
 * Prepares a registered IR off the audio thread using asynchronous file reads.
 * @param directory EffeTune data root.
 * @param parameters IR Reverb parameters from the preset.
 * @param sampleRate Engine processing rate.
 * @param processingChannels Actual selected channel count.
 * @param cache Cache location and total disk budget; empty disables persistence.
 * @param maximumMemoryBytes Combined process asset limit.
 * @return Prepared IR, dependency paths, or a fatal diagnostic.
 */
IrAssetLoadResult loadIrAsset(const std::filesystem::path &directory,
                             yyjson_val *parameters, float sampleRate,
                             std::uint32_t processingChannels,
                             const AssetCacheOptions &cache = {},
                             std::uint64_t maximumMemoryBytes = kDefaultAssetMemoryBytes);

/**
 * Prepares registered IR data on the caller's GIO dispatcher.
 * @param directory Source data root, retained by the awaiting caller.
 * @param parameters Preset parameters, retained by the awaiting caller.
 * @param sampleRate Processing rate.
 * @param processingChannels Selected processing width.
 * @param cache Persistent cache options, retained by the awaiting caller.
 * @param cancellation Superseded preparation or shutdown notification.
 * @param maximumMemoryBytes Combined process asset limit.
 * @return Owned IR result, diagnostics, and dependencies.
 */
cardio::promise<IrAssetLoadResult> loadIrAssetAsync(const std::filesystem::path &directory,
    yyjson_val *parameters, float sampleRate, std::uint32_t processingChannels,
    const AssetCacheOptions &cache, cardio::cancellation cancellation,
    std::uint64_t maximumMemoryBytes);

} // namespace pipetune
#endif
