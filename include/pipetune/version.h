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

} // namespace pipetune

#endif
