/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output-level-meter.h"
#include "localization.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>

namespace pipetune_gtk {

OutputLevelMeterWidgets createOutputLevelMeter() {
  auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
  auto *bar = gtk_level_bar_new();
  auto *value = gtk_label_new("");
  auto *channel = gtk_label_new("");
  gtk_orientable_set_orientation(GTK_ORIENTABLE(bar), GTK_ORIENTATION_VERTICAL);
  gtk_level_bar_set_inverted(GTK_LEVEL_BAR(bar), TRUE);
  gtk_level_bar_set_max_value(GTK_LEVEL_BAR(bar), 60);
  gtk_widget_set_size_request(bar, 22, 100);
  gtk_widget_set_halign(bar, GTK_ALIGN_CENTER);
  gtk_label_set_width_chars(GTK_LABEL(value), 6);
  gtk_style_context_add_class(gtk_widget_get_style_context(bar), "output-level-meter");
  gtk_box_pack_start(GTK_BOX(root), value, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), bar, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), channel, FALSE, FALSE, 0);
  return {root, bar, value, channel};
}

void updateOutputLevelMeter(const OutputLevelMeterWidgets &widgets,
    std::string_view channelLabel, std::string_view purpose,
    std::optional<float> peak) {
  const auto available = peak && std::isfinite(*peak) && *peak >= 0;
  const auto db = available && *peak > 0
      ? 20 * std::log10(static_cast<double>(*peak)) : -INFINITY;
  auto text = std::string(available ? "−∞" : "—");
  if (available && *peak > 0) {
    auto buffer = std::array<char, 32>{};
    std::snprintf(buffer.data(), buffer.size(), db > 0 ? "+%.1f" : "%.1f", db);
    text = buffer.data();
  }
  gtk_level_bar_set_value(GTK_LEVEL_BAR(widgets.levelBar), std::clamp(db + 60, 0.0, 60.0));
  gtk_label_set_text(GTK_LABEL(widgets.valueLabel), text.c_str());
  gtk_label_set_text(GTK_LABEL(widgets.channelLabel), std::string(channelLabel).c_str());
  auto *style = gtk_widget_get_style_context(widgets.levelBar);
  for (const auto *name : {"output-level-high", "output-level-overload"})
    gtk_style_context_remove_class(style, name);
  if (db >= 0) gtk_style_context_add_class(style, "output-level-overload");
  else if (db >= -6) gtk_style_context_add_class(style, "output-level-high");
  const auto name = std::string(channelLabel) + (purpose.empty() ? "" : " · " + std::string(purpose));
  const auto valueDescription = available ? text + " dBFS" : std::string(translate("Unavailable"));
  const auto accessibleName = name + ": " + valueDescription;
  const auto *description = translate("Peak output level before device volume. Silence is −∞ dBFS; 0 dBFS is full scale.");
  atk_object_set_name(gtk_widget_get_accessible(widgets.levelBar), accessibleName.c_str());
  atk_object_set_description(gtk_widget_get_accessible(widgets.levelBar), description);
  const auto tooltip = accessibleName + "\n" + description;
  gtk_widget_set_tooltip_text(widgets.root, tooltip.c_str());
}

} // namespace pipetune_gtk
