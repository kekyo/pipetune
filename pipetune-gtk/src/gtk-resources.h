#ifndef PIPETUNE_GTK_RESOURCES_H
#define PIPETUNE_GTK_RESOURCES_H

namespace pipetune_gtk {

/**
 * Registers the resources embedded in the PipeTune GTK executable.
 *
 * Repeated calls are safe and retain one process-lifetime registration.
 */
void ensureGtkResourcesRegistered() noexcept;

} // namespace pipetune_gtk

#endif
