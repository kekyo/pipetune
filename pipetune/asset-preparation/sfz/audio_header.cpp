/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * Derived from readSfzAudioHeader and parseIrAudioHeader in EffeTune v2.13.0.
 * Copyright (c) 2025-2026 Yoshiyuki Kobayashi. See LICENSE.effetune.
 */
#include "audio_header.h"
#include <cmath>
#include <string_view>

namespace pipetune::assets {

static bool available(std::span<const std::uint8_t> bytes, std::uint64_t size,
    std::uint64_t position, std::uint64_t length) {
  return position <= size && length <= size - position && position <= bytes.size() && length <= bytes.size() - position;
}

static bool tag(std::span<const std::uint8_t> bytes, std::size_t offset, std::string_view text) {
  return std::string_view(reinterpret_cast<const char *>(bytes.data() + offset), text.size()) == text;
}

static std::uint64_t word(std::span<const std::uint8_t> bytes, std::size_t offset, std::size_t length, bool little) {
  auto result = std::uint64_t{0};
  for (auto index = std::size_t{0}; index < length; ++index)
    result |= static_cast<std::uint64_t>(bytes[offset + index]) << (8 * (little ? index : length - index - 1));
  return result;
}

SfzAudioHeader inspectSfzAudioHeader(std::span<const std::uint8_t> bytes, std::uint64_t fileBytes) {
  if (!available(bytes, fileBytes, 0, 12)) return {};
  auto result = SfzAudioHeader{};
  if (tag(bytes, 0, "fLaC")) {
    if (!available(bytes, fileBytes, 0, 42) || (bytes[4] & 0x7f) != 0 || word(bytes, 5, 3, false) != 34) return {};
    const auto packed = word(bytes, 18, 8, false);
    result.sampleRate = (packed >> 44) & 0xfffff;
    result.channels = static_cast<std::uint32_t>((packed >> 41) & 7) + 1;
    result.frames = packed & 0xfffffffff;
  } else {
    const auto wav = tag(bytes, 0, "RIFF") && tag(bytes, 8, "WAVE");
    const auto aiff = tag(bytes, 0, "FORM") && (tag(bytes, 8, "AIFF") || tag(bytes, 8, "AIFC"));
    if (!wav && !aiff) return {};
    auto position = std::uint64_t{12};
    auto blockAlign = std::uint64_t{0};
    auto format = false;
    for (auto count = 0u; count < 128 && position < 1024 * 1024; ++count) {
      if (!available(bytes, fileBytes, position, 8)) break;
      const auto offset = static_cast<std::size_t>(position);
      const auto length = word(bytes, offset + 4, 4, wav);
      if (wav && tag(bytes, offset, "fmt ") && length >= 16) {
        if (!available(bytes, fileBytes, position + 8, 16)) break;
        result.channels = static_cast<std::uint32_t>(word(bytes, offset + 10, 2, true));
        result.sampleRate = word(bytes, offset + 12, 4, true);
        blockAlign = word(bytes, offset + 20, 2, true);
        format = true;
      } else if (wav && tag(bytes, offset, "data") && format) {
        result.frames = blockAlign ? length / blockAlign : 0;
        break;
      } else if (aiff && tag(bytes, offset, "COMM") && length >= 18) {
        if (!available(bytes, fileBytes, position + 8, 18)) break;
        result.channels = static_cast<std::uint32_t>(word(bytes, offset + 8, 2, false));
        result.frames = word(bytes, offset + 10, 4, false);
        const auto exponent = static_cast<std::uint32_t>(word(bytes, offset + 16, 2, false));
        const auto high = static_cast<double>(word(bytes, offset + 18, 4, false));
        const auto low = static_cast<double>(word(bytes, offset + 22, 4, false));
        const auto power = static_cast<int>(exponent & 0x7fff) - 16383;
        const auto rate = (exponent & 0x8000 ? -1 : 1) * (std::ldexp(high, power - 31) + std::ldexp(low, power - 63));
        result.sampleRate = std::floor(rate + 0.5);
        break;
      }
      position += 8 + length + (length & 1);
    }
  }
  return result.channels > 0 && result.sampleRate > 0 && result.frames > 0 ? result : SfzAudioHeader{};
}

} // namespace pipetune::assets
