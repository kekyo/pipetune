/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_FILE_H
#define PIPETUNE_ASSET_FILE_H

#include <cardio.h>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace pipetune {

/** A source identity retained until the complete candidate is ready for publication. */
struct AssetFileSnapshot {
  std::filesystem::path path; /**< Original file path. */
  std::string digest; /**< SHA-256 of the validated source bytes. */
  std::size_t bytes; /**< Exact source byte count, also the next read's allocation limit. */
};

/**
 * Verifies a set of original files immediately before candidate publication.
 * @param snapshots Identities in verification order, with selecting registries last.
 * @param cancellation Superseded preparation or shutdown notification.
 * @return Completion when every source still matches its prepared input.
 * @throws std::runtime_error An original is missing, changed, or no longer regular.
 * @remarks The caller reserves memory for the largest source; files are read one at a time.
 */
cardio::promise<void> verifyAssetSnapshots(std::span<const AssetFileSnapshot> snapshots,
                                         cardio::cancellation cancellation);

/**
 * Reads a bounded regular file using the current cardio GIO dispatcher.
 * @param path Source file; symbolic links are rejected.
 * @param maximumBytes Limit checked before allocation and after reading.
 * @param cancellation Superseded preparation or shutdown notification.
 * @return Owned, stable contents; rejects files changed during the read.
 * @throws std::runtime_error Invalid, missing, changed, or oversized input.
 */
cardio::promise<std::vector<std::uint8_t>> readAssetFile(
    const std::filesystem::path &path, std::size_t maximumBytes,
    cardio::cancellation cancellation = {});

/** Computes a lowercase SHA-256 digest for a source snapshot. */
std::string assetSha256(std::span<const std::uint8_t> bytes);

} // namespace pipetune
#endif
