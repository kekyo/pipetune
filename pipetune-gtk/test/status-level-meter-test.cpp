/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "status-level-meter.h"

#include <gtk/gtk.h>

#include <cmath>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool approximately(double actual, double expected) {
  return std::abs(actual - expected) < 0.000001;
}

int main(int argc, char **argv) {
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "GTK display is unavailable\n";
    return 1;
  }

  const auto meter = pipetune_gtk::createStatusLevelMeter();
  gtk_widget_show_all(meter.root);
  auto minimumWidth = int{0};
  auto naturalWidth = int{0};
  gtk_widget_get_preferred_width(
      meter.root, &minimumWidth, &naturalWidth);

  pipetune_gtk::updateStatusLevelMeter(
      meter,
      {
          .minimum = 0.0,
          .maximum = 100.0,
          .value = 20.0,
          .hueStep = 2,
          .valueText = "20.0%",
          .accessibleName = "Load 20.0%",
          .accessibleDescription =
              "DSP processing load; graph capped at 100%.",
      });

  auto *bar = GTK_LEVEL_BAR(meter.levelBar);
  auto *labelAccessible = gtk_widget_get_accessible(meter.valueLabel);
  auto *barAccessible = gtk_widget_get_accessible(meter.levelBar);
  auto *barContext = gtk_widget_get_style_context(meter.levelBar);
  const auto valid =
      check(minimumWidth == 150,
            "status level meter minimum width differs") &&
      check(naturalWidth == 280,
            "status level meter maximum natural width differs") &&
      check(gtk_level_bar_get_mode(bar) ==
                GTK_LEVEL_BAR_MODE_CONTINUOUS,
            "status level meter must render one continuous block") &&
      check(approximately(gtk_level_bar_get_min_value(bar), 0.0) &&
                approximately(gtk_level_bar_get_max_value(bar), 100.0) &&
                approximately(gtk_level_bar_get_value(bar), 20.0),
            "status level meter numeric range differs") &&
      check(std::string_view(
                gtk_label_get_text(GTK_LABEL(meter.valueLabel))) ==
                "20.0%",
            "status level meter value text differs") &&
      check(gtk_widget_get_halign(meter.valueLabel) == GTK_ALIGN_END &&
                gtk_widget_get_valign(meter.valueLabel) ==
                    GTK_ALIGN_CENTER &&
                gtk_widget_get_margin_end(meter.valueLabel) == 4,
            "status level meter value must align inside the right edge") &&
      check(atk_object_get_role(labelAccessible) ==
                ATK_ROLE_REDUNDANT_OBJECT,
            "visual value label must not duplicate the accessible value") &&
      check(std::string_view(atk_object_get_name(barAccessible)) ==
                "Load 20.0%" &&
                std::string_view(
                    atk_object_get_description(barAccessible)) ==
                    "DSP processing load; graph capped at 100%.",
            "status level meter accessibility text differs") &&
      check(gtk_style_context_has_class(
                barContext, "status-level-hue-2") != FALSE,
            "status level meter HUE class differs");

  gtk_widget_destroy(meter.root);
  return valid ? 0 : 1;
}
