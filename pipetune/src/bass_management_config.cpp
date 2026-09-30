/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "bass_management_config.h"
#include "dsp_catalog.h"

namespace pipetune {

BassManagementConfig decodeBassManagementConfig(const PackedParameters &packed,
                                                std::uint32_t processingChannels) {
  auto result = BassManagementConfig{};
  result.processingChannels = processingChannels;
  if (packed.floats.size() != 89u || packed.definition == nullptr ||
      packed.definition->elements.size() != packed.floats.size() ||
      processingChannels == 0 || processingChannels > 16) {
    result.error = "Bass Management parameter layout or processing width is invalid";
    return result;
  }
  // Use the generated schema's keys rather than parsing the original JSON a
  // second time: array/direct-key precedence and numeric coercion must agree
  // with the packed parameters used by the native kernel and FIR designer.
  const auto value = [&](std::string_view key, std::size_t element) {
    for (auto i = std::size_t{0}; i < packed.floats.size(); ++i) {
      const auto &field = packed.definition->elements[i];
      if ((field.arrayKey == key && field.containerIndex == element) ||
          (field.arrayKey.empty() && field.directKey == key)) return packed.floats[i];
    }
    result.error = "Bass Management parameter is absent from the native catalog";
    return 0.0F;
  };
  const auto validSlope = [](float slope) { return slope == 24 || slope == 48 || slope == 96; };
  result.linear = value("ph", 0) == 1;
  result.taps = 8192u << static_cast<std::uint32_t>(value("tp", 0));
  const auto subs = static_cast<std::uint32_t>(value("su", 0));
  const auto mask = (1u << processingChannels) - 1u;
  const auto lfeLowpass = value("lo", 0) == 1;
  const auto lfeFrequency = value("lf", 0);
  const auto lfeSlope = value("ls", 0);
  if (!validSlope(lfeSlope)) {
    result.error = "Bass Management LFE slope must be 24, 48 or 96 dB/oct";
    return result;
  }
  if ((subs & ~mask) != 0) {
    result.error = "Bass Management sub output is outside the processing channels";
    return result;
  }
  for (auto ch = 0u; ch < 16u; ++ch) {
    const auto slope = value("sl", ch);
    if (!validSlope(slope)) {
      result.error = "Bass Management channel " + std::to_string(ch + 1u) +
          " slope must be 24, 48 or 96 dB/oct";
      return result;
    }
    if (subs == 0) continue;
    const auto role = value("ro", ch);
    const auto routes = static_cast<std::uint32_t>(value("rt", ch));
    const auto inversions = static_cast<std::uint32_t>(value("ri", ch));
    if ((inversions & ~routes) != 0 || (routes & ~subs) != 0) {
      result.error = "Bass Management route inversions must target enabled routes and routes must target sub outputs";
      return result;
    }
    if (ch >= processingChannels) {
      if (role == 1 || role == 2 || routes != 0) {
        result.error = "Bass Management role or route is outside the processing channels";
        return result;
      }
      continue;
    }
    if (role <= 1 && (subs & (1u << ch)) != 0) {
      result.error = "Bass Management sub output cannot also be a Full Range or Managed channel";
      return result;
    }
    if ((role == 1 || role == 2) && routes == 0) {
      result.error = "Bass Management Managed and LFE inputs require a sub destination";
      return result;
    }
    if (role == 1 || (role == 2 && lfeLowpass)) {
      result.lowpassInputs.push_back(ch);
      result.frequencies[ch] = role == 1 ? value("fc", ch) : lfeFrequency;
      result.slopes[ch] = static_cast<std::uint32_t>(role == 1 ? slope : lfeSlope);
    }
  }
  return result;
}

} // namespace pipetune
