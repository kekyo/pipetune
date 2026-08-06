#ifndef PIPETUNE_WIREPLUMBER_VISIBILITY_H
#define PIPETUNE_WIREPLUMBER_VISIBILITY_H

#include <string_view>

namespace pipetune {

/**
 * Returns the WirePlumber policy that hides PipeTune-internal nodes.
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
