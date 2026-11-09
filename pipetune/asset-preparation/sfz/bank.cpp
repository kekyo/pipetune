/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from EffeTune v2.17.0 js/sfz/asset.js and js/sfz/schema.js.
 * https://github.com/Frieve-A/effetune/blob/v2.17.0/js/sfz/asset.js
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "bank.h"
#include "text.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

namespace pipetune::assets {

static constexpr auto kHeaderWords = std::size_t{12};
static constexpr auto kRegionWords = std::size_t{113};

// Default lanes 30 through 110 preserve the existing resident SFZ subset.
static constexpr auto kExtraDefaults = std::array<double, 81>{
    0, // trigger
    1, // lochan
    16, // hichan
    -8192, // lobend
    8192, // hibend
    0, // lochanaft
    127, // hichanaft
    0, // lopolyaft
    127, // hipolyaft
    0, // loprog
    127, // hiprog
    -1, // sw_lokey
    -1, // sw_hikey
    -1, // sw_last
    -1, // sw_lolast
    -1, // sw_hilast
    -1, // sw_down
    -1, // sw_up
    -1, // sw_previous
    -1, // sw_default
    0, // group
    0, // off_by
    0, // off_mode
    0.006, // off_time
    0, // polyphony
    0, // note_polyphony
    200, // bend_up
    -200, // bend_down
    20000, // cutoff
    0, // resonance
    0, // fil_type
    0, // ampeg_delay
    0, // ampeg_start
    0, // amp_keytrack
    60, // amp_keycenter
    0, // amp_random
    0, // pitch_veltrack
    0, // pitch_random
    0, // delay
    0, // delay_random
    0, // pitcheg_delay
    0, // pitcheg_start
    0, // pitcheg_attack
    0, // pitcheg_hold
    0, // pitcheg_decay
    0, // pitcheg_sustain
    0, // pitcheg_release
    0, // pitcheg_depth
    0, // fileg_delay
    0, // fileg_start
    0, // fileg_attack
    0, // fileg_hold
    0, // fileg_decay
    0, // fileg_sustain
    0, // fileg_release
    0, // fileg_depth
    0, // amplfo_delay
    0, // amplfo_fade
    0, // amplfo_freq
    0, // amplfo_depth
    0, // pitchlfo_delay
    0, // pitchlfo_fade
    0, // pitchlfo_freq
    0, // pitchlfo_depth
    0, // fillfo_delay
    0, // fillfo_fade
    0, // fillfo_freq
    0, // fillfo_depth
    0, // direction
    -1, // loop_count
    0, // loop_crossfade
    0, // ampeg_dynamic
    50, // eq1_freq
    1, // eq1_bw
    0, // eq1_gain
    500, // eq2_freq
    1, // eq2_bw
    0, // eq2_gain
    5000, // eq3_freq
    1, // eq3_bw
    0, // eq3_gain
};
static constexpr auto kIndexFields = std::array{0u, 1u, 20u, 21u, 23u, 24u, 111u, 112u};

struct SampleInfo {
  const Audio *audio;
  std::uint32_t frames;
  std::uint32_t channels;
  std::uint32_t offset = 0;
};

struct BankRegion {
  const SfzRegion *source;
  std::array<double, kRegionWords> fields;
};

static std::array<double, kRegionWords> regionFields(const SfzRegion &r, const SampleInfo &s) {
  const auto end = r.end.value_or(s.frames - 1);
  auto fields = std::array<double, kRegionWords>{0, static_cast<double>(s.frames), static_cast<double>(s.channels), static_cast<double>(s.audio->sampleRate),
      r.lokey, r.hikey, r.lovel, r.hivel, r.lorand, r.hirand, r.seq_length, r.seq_position,
      static_cast<double>(r.seqGroup), r.pitch_keycenter, r.pitch_keytrack, r.transpose, r.tune, r.volume, r.pan,
      r.amp_veltrack, r.offset, end, r.loop_mode, r.loop_start, r.loop_end.value_or(end),
      r.ampeg_attack, r.ampeg_hold, r.ampeg_decay, r.ampeg_sustain, r.ampeg_release};
  std::ranges::copy(kExtraDefaults, fields.begin() + 30);
  return fields;
}

SfzBank packSfzBank(std::span<const SfzRegion> regions, std::span<const SfzSample> samples,
                    std::uint64_t maximumBytes) {
  if (maximumBytes == 0 || maximumBytes > kMaximumSfzBytes) throw SfzError("too-large", "Invalid SFZ size limit.");
  if (regions.empty()) throw SfzError("no-regions", "The SFZ has no playable regions.");
  auto byPath = std::unordered_map<std::string_view, const Audio *>{};
  for (const auto &sample : samples) byPath[sample.path] = &sample.audio;
  auto information = std::map<std::u16string, SampleInfo>{};
  for (const auto &region : regions) {
    const auto key = sfzUtf16(region.sample);
    if (information.contains(key)) continue;
    const auto found = byPath.find(region.sample);
    if (found == byPath.end()) throw SfzError("prepare", "SFZ samples must contain mono or stereo audio.");
    const auto &audio = *found->second;
    if (audio.channels.empty() || audio.channels.size() > 2 || audio.channels.front().empty() ||
        audio.sampleRate < 1 || audio.sampleRate > 768000 || audio.channels.front().size() > UINT32_MAX ||
        std::ranges::any_of(audio.channels, [&](const auto &channel) { return channel.size() != audio.channels.front().size(); }))
      throw SfzError("prepare", "SFZ samples must contain mono or stereo audio.");
    information.emplace(key, SampleInfo{&audio, static_cast<std::uint32_t>(audio.channels.front().size()),
        static_cast<std::uint32_t>(audio.channels.size())});
  }
  auto result = SfzBank{};
  auto playable = std::vector<BankRegion>{};
  auto firstInvalid = std::string{};
  auto ignoredLoops = std::uint32_t{0};
  auto usedPaths = std::set<std::u16string>{};
  auto groups = std::set<std::uint32_t>{};
  for (const auto &region : regions) {
    const auto key = sfzUtf16(region.sample);
    const auto &info = information.at(key);
    auto fields = regionFields(region, info);
    try {
      if (fields[20] > fields[21] || fields[21] >= info.frames || fields[21] < 0)
        throw SfzError("prepare", "SFZ playback points exceed the sample.");
      const auto invalidLoop = fields[23] < 0 || fields[23] > fields[24] || fields[24] > fields[21];
      if (invalidLoop && (fields[22] == 0 || fields[22] == 1)) {
        fields[23] = 0;
        fields[24] = fields[21];
      } else if (invalidLoop) throw SfzError("prepare", "SFZ loop points exceed the sample.");
      for (auto field = 1u; field < kRegionWords; ++field) {
        const auto value = fields[field];
        if (!std::isfinite(value)) throw SfzError("prepare", "SFZ region has invalid values.");
        if (std::ranges::find(kIndexFields, field) != kIndexFields.end() &&
            (value < 0 || value > UINT32_MAX || std::trunc(value) != value))
          throw SfzError("prepare", "SFZ sample positions must be non-negative integers.");
      }
      if (region.lokey < 0 || region.hikey > 127 || region.hikey < region.lokey ||
          std::trunc(region.lokey) != region.lokey || std::trunc(region.hikey) != region.hikey)
        throw SfzError("prepare", "SFZ region contains an invalid key range.");
      if (invalidLoop) ++ignoredLoops;
      playable.push_back({&region, fields});
      usedPaths.insert(key);
      groups.insert(region.seqGroup);
    } catch (const SfzError &error) {
      if (firstInvalid.empty()) firstInvalid = error.what();
      result.invalidRegions.push_back(region.sample + ": " + error.what());
    }
  }
  if (playable.empty()) throw SfzError("prepare", firstInvalid);
  auto poolLength = std::uint64_t{0};
  for (const auto &path : usedPaths) {
    auto &info = information.at(path);
    if (poolLength > UINT32_MAX) throw SfzError("too-large", "The SFZ samples are too large to load.");
    info.offset = static_cast<std::uint32_t>(poolLength);
    poolLength += static_cast<std::uint64_t>(info.frames) * info.channels;
  }
  const auto poolOffset = kHeaderWords + kRegionWords * playable.size();
  const auto wordCount = poolOffset + poolLength;
  const auto byteLength = 32 + wordCount * 4;
  if (byteLength > maximumBytes || wordCount > UINT32_MAX)
    throw SfzError("too-large", "The SFZ samples are too large to load.");
  result.payload.resize(static_cast<std::size_t>(byteLength));
  result.footprintBytes = byteLength;
  result.samples = static_cast<std::uint32_t>(wordCount);
  result.regionCount = static_cast<std::uint32_t>(playable.size());
  result.warmupFrames = static_cast<std::uint32_t>((2 * playable.size() + 2 + (wordCount + 31) / 32 + 7) / 8);
  const auto put = [&](std::size_t offset, std::uint32_t word) {
    for (auto byte = 0u; byte < 4; ++byte) result.payload[offset + byte] = static_cast<std::uint8_t>(word >> (8 * byte));
  };
  put(0, 0x31415445); put(4, 1); put(8, result.samples); put(12, 1);
  const auto header = std::array<std::uint32_t, kHeaderWords>{0x53465a, 6, static_cast<std::uint32_t>(playable.size()),
      kRegionWords, static_cast<std::uint32_t>(poolOffset), result.samples, static_cast<std::uint32_t>(groups.size()), 0,
      static_cast<std::uint32_t>(poolOffset), 0, 0, 0};
  for (auto index = 0u; index < header.size(); ++index) put(32 + index * 4, header[index]);
  for (auto row = std::size_t{0}; row < playable.size(); ++row) {
    auto &region = playable[row];
    region.fields[0] = information.at(sfzUtf16(region.source->sample)).offset;
    region.fields[111] = poolOffset;
    region.fields[12] = std::distance(groups.begin(), groups.find(region.source->seqGroup));
    for (auto field = 0u; field < kRegionWords; ++field) {
      const auto bits = std::ranges::find(kIndexFields, field) != kIndexFields.end() ?
          static_cast<std::uint32_t>(region.fields[field]) : std::bit_cast<std::uint32_t>(static_cast<float>(region.fields[field]));
      put(32 + (kHeaderWords + row * kRegionWords + field) * 4, bits);
    }
  }
  for (const auto &path : usedPaths) {
    const auto &info = information.at(path);
    auto cursor = static_cast<std::size_t>(poolOffset + info.offset);
    for (auto frame = 0u; frame < info.frames; ++frame)
      for (const auto &channel : info.audio->channels) {
        const auto value = channel[frame];
        if (!std::isfinite(value)) throw SfzError("prepare", "SFZ audio contains invalid samples.");
        put(32 + cursor++ * 4, std::bit_cast<std::uint32_t>(value));
      }
  }
  if (ignoredLoops) result.warnings.push_back({"loop-points-ignored", ignoredLoops});
  if (!result.invalidRegions.empty()) result.warnings.push_back({"invalid-regions", static_cast<std::uint32_t>(result.invalidRegions.size())});
  return result;
}

} // namespace pipetune::assets
