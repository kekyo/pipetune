/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_ASSET_LOADER_H
#define PIPETUNE_ROOM_ASSET_LOADER_H
#include "asset_file.h"
#include "asset_preparation_resources.h"
#include <pipetune/asset_cache.h>
#include <yyjson.h>

namespace pipetune {

/** Result of resolving measurements and designing a Room EQ convolution asset. */
struct RoomAssetLoadResult {
  std::shared_ptr<const SharedPreparedAsset> prepared; /**< Immutable coefficients and diagnostics. */
  std::vector<std::filesystem::path> files; /**< Source dependencies, including missing measurements. */
  std::vector<AssetFileSnapshot> snapshots; /**< Original identities to verify before publication. */
  std::string error; /**< Fatal diagnostic, or empty on success. */
  bool cacheHit = false; /**< Whether a completed design was reused. */
  std::uint32_t channelDelaySamples = 0; /**< Saved millisecond delay regenerated at the processing rate. */
};

/**
 * Reads saved measurements and prepares channel FIRs on the caller's GIO dispatcher.
 * @param directory Measurement-backup directory.
 * @param parameters Room EQ preset parameters retained by the caller.
 * @param sampleRate Engine processing sample rate.
 * @param processingChannels Number of selected output channels.
 * @param cache Persistent cache location and disk budget.
 * @param cancellation Superseded preparation or shutdown notification.
 * @param maximumMemoryBytes Combined process asset budget.
 * @return Owned prepared asset, all dependencies, or a fatal diagnostic.
 */
cardio::promise<RoomAssetLoadResult> loadRoomAssetAsync(const std::filesystem::path &directory,
    yyjson_val *parameters, float sampleRate, std::uint32_t processingChannels,
    const AssetCacheOptions &cache, cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes);

} // namespace pipetune
#endif
