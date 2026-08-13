/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_STATUS_LEVEL_METER_H
#define PIPETUNE_GTK_STATUS_LEVEL_METER_H

#include <gtk/gtk.h>

#include <cstdint>
#include <string_view>

namespace pipetune_gtk {

/**
 * Holds the widgets used to render one bounded numeric status level.
 */
struct StatusLevelMeterWidgets {
  /** Full-width host packed into the status value stack. */
  GtkWidget *root;
  /** Continuous level indicator constrained inside the host. */
  GtkWidget *levelBar;
  /** Formatted current value overlaid at the right edge. */
  GtkWidget *valueLabel;
};

/**
 * Describes one visual and accessible update of a status level meter.
 */
struct StatusLevelMeterState {
  /** Inclusive lower bound represented by an empty bar. */
  double minimum;
  /** Inclusive upper bound represented by a full bar. */
  double maximum;
  /** Current numeric value, clamped to minimum and maximum for drawing. */
  double value;
  /** Low-saturation HUE step from zero through ten. */
  std::uint8_t hueStep;
  /** Existing display-ready status value shown inside the bar. */
  std::string_view valueText;
  /** Complete accessible name including the status label and value. */
  std::string_view accessibleName;
  /** Accessible explanation of the represented range. */
  std::string_view accessibleDescription;
};

/**
 * Creates a right-aligned status level meter.
 *
 * The meter requests a 150-pixel minimum and a 280-pixel natural width.
 * Its full-width host lets the meter grow with the status pane until the
 * natural width is reached.
 *
 * @return Newly allocated widget tree owned by its eventual GTK parent.
 */
StatusLevelMeterWidgets createStatusLevelMeter();

/**
 * Updates the bounded level, formatted text, HUE, and accessibility.
 *
 * Invalid numeric ranges fall back to a zero-to-one range and an empty bar.
 *
 * @param widgets Meter widgets returned by createStatusLevelMeter().
 * @param state Complete display state for the next frame.
 */
void updateStatusLevelMeter(const StatusLevelMeterWidgets &widgets,
                            const StatusLevelMeterState &state);

} // namespace pipetune_gtk

#endif
