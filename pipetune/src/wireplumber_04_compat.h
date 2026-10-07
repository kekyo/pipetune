/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_WIREPLUMBER_04_COMPAT_H
#define PIPETUNE_WIREPLUMBER_04_COMPAT_H

#include <string_view>

namespace pipetune {

/**
 * Returns the WirePlumber 0.4 policy configuration managed by PipeTune.
 *
 * @return Complete policy.lua.d fragment contents.
 */
std::string_view wirePlumber04CompatibilityPolicy() noexcept;

/**
 * Returns the WirePlumber 0.4 stream configuration for public PipeTune inputs.
 *
 * @return Complete main.lua.d fragment contents.
 * @remarks PipeTune retains its master controls while rebuilding processing
 * nodes. The session manager must not replace them with saved stream defaults.
 */
std::string_view wirePlumber04StreamConfiguration() noexcept;

/**
 * Returns the WirePlumber 0.4 endpoint-client compatibility script.
 *
 * @return Complete runtime Lua script contents.
 */
std::string_view wirePlumber04EndpointClientPolicy() noexcept;

/**
 * Returns the WirePlumber 0.4 endpoint-device compatibility script.
 *
 * @return Complete runtime Lua script contents.
 */
std::string_view wirePlumber04EndpointDevicePolicy() noexcept;

} // namespace pipetune

#endif
