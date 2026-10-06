/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_WIREPLUMBER_VISIBILITY_H
#define PIPETUNE_WIREPLUMBER_VISIBILITY_H

#include <string_view>

namespace pipetune {

/**
 * Returns the WirePlumber policy for PipeTune node visibility and default output.
 *
 * Internal nodes remain private. Aggregate outputs can request a temporary
 * default-output selection whose previous preference survives their removal
 * and a WirePlumber restart.
 *
 * @return Complete runtime Lua script contents.
 */
std::string_view wirePlumberNodeVisibilityPolicy() noexcept;

/**
 * Returns the WirePlumber 0.5 component configuration for the policy.
 *
 * @return Complete wireplumber.conf.d fragment contents.
 */
std::string_view wirePlumber05NodeVisibilityConfiguration() noexcept;

} // namespace pipetune

#endif
