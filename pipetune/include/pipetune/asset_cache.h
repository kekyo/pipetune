/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_CACHE_OPTIONS_H
#define PIPETUNE_ASSET_CACHE_OPTIONS_H
#include <cstdint>
#include <filesystem>

namespace pipetune {

/** Persistent preparation cache configuration; source data is always validated separately. */
struct AssetCacheOptions {
  std::filesystem::path directory; /**< PipeTune cache root; empty disables persistence. */
  std::uint64_t maximumBytes = 2ull * 1024 * 1024 * 1024; /**< Total disk budget, including metadata. */
};

} // namespace pipetune
#endif
