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
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pipetune {

/** A source identity retained until the complete candidate is ready for publication. */
struct AssetFileSnapshot {
  std::filesystem::path path; /**< Original file path. */
  std::string digest; /**< SHA-256 of the validated source bytes. */
  std::size_t bytes; /**< Exact source byte count, also the next read's allocation limit. */
  std::filesystem::path root = {}; /**< Registered canonical folder, empty for an unconfined source. */
  std::filesystem::path resolvedPath = {}; /**< Canonical target selected inside root. */
  bool missing = false; /**< Whether a missing optional sample must remain missing. */
};

/** A complete content identity with only a bounded prefix retained for metadata parsing. */
struct AssetFileInspection {
  AssetFileSnapshot snapshot; /**< Complete source size and SHA-256. */
  std::vector<std::uint8_t> prefix; /**< At most the requested number of initial bytes. */
};

/**
 * Streams a regular file and hashes all bytes without retaining its full contents.
 * @param path Canonical source path; final symbolic links are rejected.
 * @param maximumBytes Maximum admitted complete file size.
 * @param prefixBytes Maximum initial bytes retained in memory.
 * @param cancellation Superseded preparation or shutdown notification.
 * @return Stable source identity and bounded prefix.
 * @throws std::runtime_error Invalid, changed, or oversized source.
 */
cardio::promise<AssetFileInspection> inspectAssetFile(const std::filesystem::path &path,
    std::size_t maximumBytes, std::size_t prefixBytes, cardio::cancellation cancellation);

/**
 * Resolves a source using asynchronous component queries and canonical folder confinement.
 * @param root Registered absolute canonical folder, which must not have moved through a link.
 * @param relativePath Normalized path relative to that folder.
 * @param dependencies Appended original, link, and target paths needed for change monitoring.
 * @param cancellation Superseded preparation or shutdown notification.
 * @return Canonical existing path, or no path for a missing dependency.
 * @throws std::runtime_error An invalid path, moved root, link cycle, or target outside root.
 */
cardio::promise<std::optional<std::filesystem::path>> resolveConfinedAssetPath(
    const std::filesystem::path &root, const std::filesystem::path &relativePath,
    std::vector<std::filesystem::path> &dependencies, cardio::cancellation cancellation);

/**
 * Verifies a set of original files immediately before candidate publication.
 * @param snapshots Identities in verification order, with selecting registries last.
 * @param cancellation Superseded preparation or shutdown notification.
 * @return Completion when every source still matches its prepared input.
 * @throws std::runtime_error An original is missing, changed, or no longer regular.
 * @remarks Sources are streamed one at a time, using a 64 KiB read buffer.
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
