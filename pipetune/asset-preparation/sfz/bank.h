/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_BANK_H
#define PIPETUNE_SFZ_BANK_H
#include "region.h"
#include <span>

namespace pipetune::assets {

/** Owned ETA1/table-v2 bank and the metadata needed by the native copy boundary. */
struct SfzBank {
  std::vector<std::uint8_t> payload; /**< Header, normalized region table, and interleaved PCM pool. */
  std::uint64_t footprintBytes = 0; /**< Bank plus native key and round-robin indexes. */
  std::uint32_t samples = 0; /**< Number of four-byte words following the 32-byte outer header. */
  std::uint32_t warmupFrames = 0; /**< Minimum native preparation frames. */
  std::uint32_t regionCount = 0; /**< Regions surviving sample-position validation. */
  std::vector<SfzWarning> warnings; /**< Invalid-region and unused-loop-point conditions. */
  std::vector<std::string> invalidRegions; /**< Details for skipped sample-position errors. */
};

/**
 * Encodes a bank using original-rate PCM, including integer bit lanes and the native index budget.
 * @param regions Normalized regions in definition order.
 * @param samples Decoded sources identified by normalized paths.
 * @param maximumBytes Maximum bank and native index footprint, at most 1 GiB.
 * @return Complete bank and non-fatal region diagnostics.
 * @throws SfzError No playable region, invalid audio, or an exceeded footprint budget.
 */
SfzBank packSfzBank(std::span<const SfzRegion> regions, std::span<const SfzSample> samples,
                    std::uint64_t maximumBytes);

} // namespace pipetune::assets
#endif
