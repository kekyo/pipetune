#include "gtk-resources.h"

// The generated header lacks G_BEGIN_DECLS, so preload GIO before applying
// C linkage to its resource entry points.
#include <gio/gio.h>

extern "C" {
#include "pipetune-gtk-resources.h"
}

namespace pipetune_gtk {

void ensureGtkResourcesRegistered() noexcept {
  static const auto registered = [] {
    pipetune_gtk_resources_register_resource();
    return true;
  }();
  static_cast<void>(registered);
}

} // namespace pipetune_gtk
