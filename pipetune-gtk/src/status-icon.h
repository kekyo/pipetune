#ifndef PIPETUNE_GTK_STATUS_ICON_H
#define PIPETUNE_GTK_STATUS_ICON_H

#include "application-state.h"
#include "tray-backend.h"

namespace pipetune_gtk {

/**
 * Identifies the semantic badge overlaid on the PipeTune status artwork.
 */
enum class StatusBadge {
  /** No badge is needed for a healthy connection. */
  none,
  /** The connected runtime requires attention. */
  attention,
  /** The daemon is connecting or unavailable. */
  disconnected
};

/**
 * Describes the PipeTune artwork and badge for one application state.
 */
struct StatusIconPresentation {
  /** Whether the source artwork retains its colors. */
  TrayIconColorMode colorMode;
  /** Semantic badge overlaid on the artwork. */
  StatusBadge badge;
};

/**
 * Selects the main-window and tray artwork presentation.
 *
 * @param state Current display-independent application state.
 * @return Color mode and semantic status badge.
 */
StatusIconPresentation
statusIconPresentation(const ApplicationState &state);

} // namespace pipetune_gtk

#endif
