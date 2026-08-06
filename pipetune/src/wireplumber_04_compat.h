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
