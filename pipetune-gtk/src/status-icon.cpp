/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "status-icon.h"

namespace pipetune_gtk {

StatusIconPresentation
statusIconPresentation(const ApplicationState &state) {
  const auto colorMode =
      isPresetApplied(state) ? TrayIconColorMode::color
                             : TrayIconColorMode::grayscale;
  const auto visual = trayVisualState(state);
  if (visual == TrayVisualState::attention) {
    return {.colorMode = colorMode,
            .badge = StatusBadge::attention};
  }
  if (visual == TrayVisualState::disconnected) {
    return {.colorMode = colorMode,
            .badge = StatusBadge::disconnected};
  }
  return {.colorMode = colorMode, .badge = StatusBadge::none};
}

} // namespace pipetune_gtk
