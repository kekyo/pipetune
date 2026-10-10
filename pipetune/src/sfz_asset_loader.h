/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_ASSET_LOADER_H
#define PIPETUNE_SFZ_ASSET_LOADER_H
#include "asset_preparation_resources.h"
#include "asset_file.h"
#include <pipetune/asset_cache.h>
#include <yyjson.h>

namespace pipetune {

/** Registered SFZ preparation result, including dependencies needed after a failed load. */
struct SfzAssetLoadResult {
  std::shared_ptr<const SharedPreparedAsset> prepared; /**< Shared native bank and active diagnostics. */
  std::vector<std::filesystem::path> files; /**< Registry, source paths, and symbolic-link dependencies. */
  std::vector<AssetFileSnapshot> snapshots; /**< Complete source identities and omitted missing samples. */
  std::string error; /**< Fatal diagnostic, empty on success. */
  bool cacheHit = false; /**< Whether a completed bank was reused. */
};

/**
 * Resolves a desktop SFZ ID, prepares source files, and reuses validated native banks.
 * @param directory EffeTune data root containing sfz-references.json.
 * @param parameters Preset parameters retained by the awaiting caller.
 * @param maximumBytes Bank, decoded PCM, and original-file budget, at most 1 GiB.
 * @param cache Persistent cache options.
 * @param cancellation Superseded preparation or shutdown notification.
 * @param maximumMemoryBytes Combined process asset limit.
 * @return Owned preparation, dependency snapshots, active warnings, or a fatal diagnostic.
 * @remarks Uses the current cardio GIO dispatcher. SFZ semantics remain in the pure module.
 */
cardio::promise<SfzAssetLoadResult> loadSfzAssetAsync(const std::filesystem::path &directory,
    yyjson_val *parameters, std::uint64_t maximumBytes, const AssetCacheOptions &cache,
    cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes);

} // namespace pipetune
#endif
