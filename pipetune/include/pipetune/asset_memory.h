/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PUBLIC_ASSET_MEMORY_H
#define PIPETUNE_PUBLIC_ASSET_MEMORY_H
#include <cstdint>

namespace pipetune {

/** Process-wide asset reservation limit, leaving address space available on 32-bit hosts. */
inline constexpr std::uint64_t kDefaultAssetMemoryBytes =
    sizeof(void *) >= 8 ? 4ull * 1024 * 1024 * 1024 : 1024ull * 1024 * 1024;

} // namespace pipetune
#endif
