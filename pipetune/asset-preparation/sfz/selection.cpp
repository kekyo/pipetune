/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from EffeTune v2.13.0 js/sfz/service.js.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "selection.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <numeric>
#include <unordered_map>
#include <unordered_set>

namespace pipetune::assets {

using Metadata = std::unordered_map<std::string_view, const SfzSampleMetadata *>;

[[noreturn]] static void cannotFitRange() {
  throw SfzError("too-large", "The SFZ cannot fit its playable note range.");
}

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
  auto footprint = std::uint64_t{64 + 4 * 129};
  if (raw > maximumBytes || footprint > maximumBytes) return false;
  for (const auto &region : regions) {
    if (!addWithin(footprint, 120 + 4 * static_cast<std::uint64_t>(region.hikey - region.lokey + 1), maximumBytes)) return false;
    if (groups.insert(region.seqGroup).second && !addWithin(footprint, 8, maximumBytes)) return false;
    if (!paths.insert(region.sample).second) continue;
    const auto found = metadata.find(region.sample);
    if (found == metadata.end() || !addWithin(raw, found->second->size, maximumBytes) ||
        !addWithin(footprint, pcmBytes(*found->second), maximumBytes)) return false;
  }
  return true;
}

SfzSelection selectSfzRegionsForBudget(std::span<const SfzRegion> regions,
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
  if (fits(regions, metadata, maximumBytes, definitionBytes)) return {{regions.begin(), regions.end()}, false, 0, 0};
  auto byKey = std::array<std::vector<const SfzRegion *>, 128>{};
  auto originalKeys = std::array<bool, 128>{};
  for (const auto &region : regions) {
    const auto found = metadata.find(region.sample);
    const auto usable = found != metadata.end() && found->second->frames > 0 &&
        found->second->channels > 0 && found->second->channels <= 2;
    for (auto key = static_cast<unsigned>(region.lokey); key <= region.hikey; ++key) {
      originalKeys[key] = true;
      if (usable) byKey[key].push_back(&region);
    }
  }
  auto keyCount = std::uint32_t{0};
  for (auto key = 0u; key < 128; ++key) {
    if (!byKey[key].empty()) ++keyCount;
    else if (originalKeys[key]) cannotFitRange();
  }
  if (!keyCount) cannotFitRange();
  auto velocities = std::array<int, 127>{};
  std::iota(velocities.begin(), velocities.end(), 1);
  std::ranges::sort(velocities, [](int a, int b) {
    const auto left = std::abs(a - 64), right = std::abs(b - 64);
    return left == right ? a < b : left < right;
  });
  for (const auto velocity : velocities) {
    auto selected = std::vector<SfzRegion>{};
    const SfzRegion *previous = nullptr;
    for (auto key = 0u; key < 128; ++key) {
      const SfzRegion *best = nullptr;
      auto bestDistance = std::numeric_limits<double>::infinity();
      auto bestBytes = std::numeric_limits<std::uint64_t>::max();
      for (const auto *region : byKey[key]) {
        const auto distance = velocity < region->lovel ? region->lovel - velocity :
            velocity > region->hivel ? velocity - region->hivel : 0;
        const auto bytes = pcmBytes(*metadata.at(region->sample));
        // An exact tie retains the first source region, not the lexicographically first file.
        if (distance < bestDistance || (distance == bestDistance && bytes < bestBytes)) {
          best = region; bestDistance = distance; bestBytes = bytes;
        }
      }
      if (!best) continue;
      if (best == previous && selected.back().hikey == key - 1) selected.back().hikey = key;
      else {
        auto region = *best;
        region.lokey = region.hikey = key;
        region.lovel = 1; region.hivel = 127;
        region.lorand = 0; region.hirand = 1;
        region.seq_length = region.seq_position = 1;
        selected.push_back(std::move(region));
      }
      previous = best;
    }
    if (fits(selected, metadata, maximumBytes, definitionBytes))
      return {std::move(selected), true, static_cast<std::uint32_t>(velocity), keyCount};
  }
  cannotFitRange();
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
