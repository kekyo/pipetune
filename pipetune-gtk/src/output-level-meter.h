/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_OUTPUT_LEVEL_METER_H
#define PIPETUNE_GTK_OUTPUT_LEVEL_METER_H

#include <gtk/gtk.h>
#include <optional>
#include <string_view>

namespace pipetune_gtk {

/** Widgets showing the measured peak of one logical output channel. */
struct OutputLevelMeterWidgets {
  /** Channel column owned by its eventual GTK parent. */
  GtkWidget *root;
  /** Vertical bar representing -60 through 0 dBFS as values 0 through 60. */
  GtkWidget *levelBar;
  /** Current peak in dBFS, silence, or unavailable marker. */
  GtkWidget *valueLabel;
  /** Stable logical channel number. */
  GtkWidget *channelLabel;
};

/**
 * Creates an output channel peak meter.
 * @return Widget tree owned by its eventual GTK parent.
 */
OutputLevelMeterWidgets createOutputLevelMeter();

/**
 * Displays the latest measured peak without altering audio settings.
 * @param widgets Meter to update.
 * @param channelLabel Display-ready logical channel number.
 * @param purpose Optional user label shown in the tooltip and accessible name.
 * @param peak Linear amplitude, zero for silence, or no value when unavailable.
 */
void updateOutputLevelMeter(const OutputLevelMeterWidgets &widgets,
    std::string_view channelLabel, std::string_view purpose,
    std::optional<float> peak);

} // namespace pipetune_gtk
#endif
