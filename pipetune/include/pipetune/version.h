/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_VERSION_H
#define PIPETUNE_VERSION_H

#include <string_view>

namespace pipetune {

/**
 * Returns the PipeTune semantic version.
 *
 * @return A process-lifetime string containing the current version.
 */
std::string_view version() noexcept;

/**
 * Returns the semantic version of the embedded EffeTune DSP sources.
 *
 * @return A process-lifetime string containing the EffeTune version.
 */
std::string_view effetuneVersion() noexcept;

} // namespace pipetune

#endif
