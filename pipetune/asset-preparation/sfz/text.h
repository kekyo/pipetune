/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_TEXT_H
#define PIPETUNE_SFZ_TEXT_H
#include <string>
#include <string_view>
namespace pipetune::assets {
/** Converts valid UTF-8 into the JavaScript UTF-16 units used for string ordering and length limits. */
std::u16string sfzUtf16(std::string_view text);
}
#endif
