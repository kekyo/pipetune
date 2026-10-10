/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "text.h"
#include "region.h"

namespace pipetune::assets {

std::u16string sfzUtf16(std::string_view text) {
  auto units = std::u16string{};
  for (auto index = std::size_t{0}; index < text.size();) {
    auto code = static_cast<unsigned char>(text[index++]);
    const auto count = code < 0x80 ? 0u : code >= 0xc2 && code < 0xe0 ? 1u :
                       code >= 0xe0 && code < 0xf0 ? 2u : code >= 0xf0 && code < 0xf5 ? 3u : 4u;
    if (count == 4 || count > text.size() - index) throw SfzError("prepare", "SFZ text is not valid UTF-8.");
    auto point = std::uint32_t(code & (count == 0 ? 0x7f : count == 1 ? 0x1f : count == 2 ? 0x0f : 0x07));
    for (auto byte = 0u; byte < count; ++byte) {
      const auto next = static_cast<unsigned char>(text[index++]);
      if ((next & 0xc0) != 0x80) throw SfzError("prepare", "SFZ text is not valid UTF-8.");
      point = (point << 6) | (next & 0x3f);
    }
    if ((count == 1 && point < 0x80) || (count == 2 && point < 0x800) || (count == 3 && point < 0x10000) ||
        point > 0x10ffff || (point >= 0xd800 && point <= 0xdfff))
      throw SfzError("prepare", "SFZ text is not valid UTF-8.");
    if (point < 0x10000) units.push_back(static_cast<char16_t>(point));
    else {
      units.push_back(static_cast<char16_t>(0xd800 + ((point - 0x10000) >> 10)));
      units.push_back(static_cast<char16_t>(0xdc00 + ((point - 0x10000) & 0x3ff)));
    }
  }
  return units;
}

} // namespace pipetune::assets
