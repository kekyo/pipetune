/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from EffeTune v2.17.0 js/sfz/service.js.
 * https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/service.js
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "selection.h"
#include "bank.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <unordered_map>
#include <unordered_set>

namespace pipetune::assets {

using Metadata = std::unordered_map<std::string_view, const SfzSampleMetadata *>;

static std::uint64_t pcmBytes(const SfzSampleMetadata &info) {
  constexpr auto limit = std::numeric_limits<std::uint64_t>::max();
  const auto width = static_cast<std::uint64_t>(info.channels) * sizeof(float);
  return width && info.frames > limit / width ? limit : info.frames * width;
}

static bool addWithin(std::uint64_t &total, std::uint64_t bytes, std::uint64_t maximum) {
  if (total > maximum || bytes > maximum - total) return false;
  total += bytes;
  return true;
}

static bool fits(std::span<const SfzRegion> regions, const Metadata &metadata,
    std::uint64_t maximumBytes, std::uint64_t definitionBytes) {
  auto paths = std::unordered_set<std::string_view>{};
  auto groups = std::unordered_set<std::uint32_t>{};
  auto raw = definitionBytes;
  auto footprint = std::uint64_t{32 + 4 * kSfzTableHeaderWords + 4 * 129};
  if (raw > maximumBytes || footprint > maximumBytes) return false;
  for (const auto &region : regions) {
    if (!addWithin(footprint, 4 * kSfzRegionWords + 4 * static_cast<std::uint64_t>(region.hikey - region.lokey + 1), maximumBytes)) return false;
    if (groups.insert(region.seqGroup).second && !addWithin(footprint, 8, maximumBytes)) return false;
    paths.insert(region.sample);
  }
  for (const auto &[path, info] : metadata) {
    static_cast<void>(info);
    paths.insert(path);
  }
  for (const auto path : paths) {
    const auto found = metadata.find(path);
    if (found == metadata.end() || !addWithin(raw, found->second->size, maximumBytes) ||
        !addWithin(footprint, pcmBytes(*found->second), maximumBytes)) return false;
  }
  return true;
}

void validateSfzBudget(std::span<const SfzRegion> regions,
    std::span<const SfzSampleMetadata> samples, std::uint64_t maximumBytes, std::uint64_t definitionBytes) {
  if (maximumBytes == 0 || maximumBytes > kMaximumSfzBytes) throw SfzError("too-large", "Invalid SFZ size limit.");
  auto metadata = Metadata{};
  for (const auto &sample : samples) {
    if (!metadata.emplace(sample.path, &sample).second) throw SfzError("prepare", "Duplicate SFZ sample metadata.");
  }
  for (const auto &region : regions) {
    if (!std::isfinite(region.lokey) || !std::isfinite(region.hikey) || region.lokey < 0 ||
        region.hikey > 127 || region.lokey > region.hikey || std::trunc(region.lokey) != region.lokey ||
        std::trunc(region.hikey) != region.hikey || !std::isfinite(region.lovel) || !std::isfinite(region.hivel) ||
        region.lovel < 1 || region.hivel > 127 || region.lovel > region.hivel ||
        std::trunc(region.lovel) != region.lovel || std::trunc(region.hivel) != region.hivel)
      throw SfzError("prepare", "SFZ region contains an invalid parameter range.");
  }
  if (!fits(regions, metadata, maximumBytes, definitionBytes))
    throw SfzError("too-large", "The complete SFZ instrument exceeds the memory limit.");
}

std::vector<SfzWarning> mergeSfzWarnings(std::span<const SfzWarning> first, std::span<const SfzWarning> second) {
  auto counts = std::map<std::string, std::uint32_t>{};
  for (const auto warnings : {first, second})
    for (const auto &warning : warnings) counts[warning.code] = std::max(counts[warning.code], warning.count);
  auto result = std::vector<SfzWarning>{};
  for (const auto &[code, count] : counts) result.push_back({code, count});
  return result;
}

} // namespace pipetune::assets
