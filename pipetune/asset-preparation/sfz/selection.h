/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_SELECTION_H
#define PIPETUNE_SFZ_SELECTION_H
#include "region.h"
#include <span>

namespace pipetune::assets {

/** Source file and optional audio-header dimensions; no PCM is allocated for validation. */
struct SfzSampleMetadata {
  std::string path; /**< Normalized root-relative sample identity. */
  std::uint64_t size; /**< Encoded file byte length. */
  std::uint64_t frames; /**< Source frames, zero when the header does not provide them. */
  std::uint32_t channels; /**< Source channels, zero when unknown. */
};

/**
 * Validates the complete instrument against the original-file and bank/index budgets.
 * @param regions Normalized, playable regions in source order.
 * @param metadata Encoded sizes and optional header dimensions, with unique paths.
 * @param maximumBytes Budget from one byte through 1 GiB.
 * @param definitionBytes Unique definition/include file bytes already retained.
 * @throws SfzError Invalid input or a budget unable to cover every region and source.
 * @remarks All supplied source metadata counts, even when no region uses it. Header
 * estimates are checked again against decoded PCM and native preparation by the host.
 */
void validateSfzBudget(std::span<const SfzRegion> regions,
    std::span<const SfzSampleMetadata> metadata, std::uint64_t maximumBytes, std::uint64_t definitionBytes);

/**
 * Combines repeated import warnings without counting the same omission twice.
 * @param first Existing warnings.
 * @param second New warnings; each category keeps the greater count.
 * @return Warnings sorted by category.
 */
std::vector<SfzWarning> mergeSfzWarnings(std::span<const SfzWarning> first, std::span<const SfzWarning> second);

} // namespace pipetune::assets
#endif
