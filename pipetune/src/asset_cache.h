/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_CACHE_H
#define PIPETUNE_ASSET_CACHE_H
#include <pipetune/asset_cache.h>
#include "prepared_dsp_asset.h"
#include <cardio.h>
#include <memory>
#include <optional>
#include <string_view>

namespace pipetune {

/** Keeps one cache entry in use without retaining another copy of its PCM. */
struct AssetCachePin {
  std::filesystem::path path; /**< Owned cache path protected from this process's eviction. */
};

/** Validated cached data and its preparation diagnostics. */
struct CachedDspAsset {
  PreparedDspAsset asset; /**< Owned bytes and copy-ABI metadata. */
  std::vector<std::string> diagnostics; /**< Conditions retained when the asset is reused. */
  std::shared_ptr<const AssetCachePin> pin; /**< Entry protection for a prepared pipeline. */
};

/** Cache persistence outcome; a failure does not invalidate the prepared asset. */
struct AssetCacheWriteResult {
  std::shared_ptr<const AssetCachePin> pin; /**< Protected entry on success. */
  std::string error; /**< Non-fatal persistence diagnostic. */
};

/**
 * Reads a checksummed entry asynchronously and updates its last-use time.
 * @param options Directory and disk budget.
 * @param key Lowercase SHA-256 of source, processor version, and preparation options.
 * @param maximumPayloadBytes Largest acceptable encoded asset.
 * @param cancellation Preparation cancellation notification.
 * @return Validated entry, or no value for an absent, obsolete, or corrupt entry.
 */
cardio::promise<std::optional<CachedDspAsset>> readAssetCache(
    const AssetCacheOptions &options, std::string_view key, std::size_t maximumPayloadBytes,
    cardio::cancellation cancellation = {});

/**
 * Atomically persists prepared data and reclaims least recently used unpinned entries.
 * @param options Directory and total disk budget.
 * @param key Lowercase SHA-256 preparation identity.
 * @param asset Encoded asset retained by the caller until completion.
 * @param diagnostics Preparation conditions to restore on a later cache hit.
 * @param cancellation Preparation cancellation notification.
 * @return Protected cache entry or a non-fatal persistence error.
 */
cardio::promise<AssetCacheWriteResult> writeAssetCache(
    const AssetCacheOptions &options, std::string_view key, const PreparedDspAsset &asset,
    const std::vector<std::string> &diagnostics, cardio::cancellation cancellation = {});

} // namespace pipetune
#endif
