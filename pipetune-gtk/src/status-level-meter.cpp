#include "status-level-meter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>

namespace pipetune_gtk {

constexpr auto kStatusLevelMinimumWidth = 150;
constexpr auto kStatusLevelMaximumWidth = 280;
constexpr auto kStatusLevelHueCount = std::size_t{11};

struct StatusLevelWidthBin {
  GtkBin parent;
};

struct StatusLevelWidthBinClass {
  GtkBinClass parentClass;
};

G_DEFINE_TYPE(StatusLevelWidthBin, status_level_width_bin, GTK_TYPE_BIN)

static void statusLevelWidthBinPreferredWidth(GtkWidget *,
                                              gint *minimumWidth,
                                              gint *naturalWidth) {
  *minimumWidth = kStatusLevelMinimumWidth;
  *naturalWidth = kStatusLevelMaximumWidth;
}

static void status_level_width_bin_class_init(
    StatusLevelWidthBinClass *widgetClass) {
  auto *gtkWidgetClass = GTK_WIDGET_CLASS(widgetClass);
  gtkWidgetClass->get_preferred_width =
      statusLevelWidthBinPreferredWidth;
}

static void status_level_width_bin_init(StatusLevelWidthBin *) {}

static GtkWidget *createStatusLevelWidthBin() {
  return GTK_WIDGET(
      g_object_new(status_level_width_bin_get_type(), nullptr));
}

static void addStyleClass(GtkWidget *widget, const char *name) {
  gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static const std::array<const char *, kStatusLevelHueCount>
    kStatusLevelHueClasses = {
        "status-level-hue-0", "status-level-hue-1",
        "status-level-hue-2", "status-level-hue-3",
        "status-level-hue-4", "status-level-hue-5",
        "status-level-hue-6", "status-level-hue-7",
        "status-level-hue-8", "status-level-hue-9",
        "status-level-hue-10",
};

static void setHueStep(GtkWidget *levelBar, std::uint8_t requestedStep) {
  auto *context = gtk_widget_get_style_context(levelBar);
  for (const auto *name : kStatusLevelHueClasses) {
    gtk_style_context_remove_class(context, name);
  }
  const auto step = std::min(
      static_cast<std::size_t>(requestedStep),
      kStatusLevelHueClasses.size() - 1);
  gtk_style_context_add_class(context, kStatusLevelHueClasses[step]);
}

StatusLevelMeterWidgets createStatusLevelMeter() {
  auto *root = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
  gtk_widget_set_hexpand(root, TRUE);

  auto *widthBin = createStatusLevelWidthBin();
  auto *overlay = gtk_overlay_new();
  auto *levelBar = gtk_level_bar_new();
  gtk_orientable_set_orientation(GTK_ORIENTABLE(levelBar),
                                 GTK_ORIENTATION_HORIZONTAL);
  gtk_level_bar_set_mode(GTK_LEVEL_BAR(levelBar),
                         GTK_LEVEL_BAR_MODE_CONTINUOUS);
  addStyleClass(levelBar, "status-level-meter");
  setHueStep(levelBar, 0);
  gtk_container_add(GTK_CONTAINER(overlay), levelBar);

  auto *valueLabel = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(valueLabel), 1.0F);
  gtk_label_set_ellipsize(GTK_LABEL(valueLabel), PANGO_ELLIPSIZE_END);
  gtk_widget_set_halign(valueLabel, GTK_ALIGN_END);
  gtk_widget_set_valign(valueLabel, GTK_ALIGN_CENTER);
  gtk_widget_set_margin_end(valueLabel, 4);
  addStyleClass(valueLabel, "status-level-value");
  atk_object_set_role(gtk_widget_get_accessible(valueLabel),
                      ATK_ROLE_REDUNDANT_OBJECT);
  gtk_overlay_add_overlay(GTK_OVERLAY(overlay), valueLabel);

  gtk_container_add(GTK_CONTAINER(widthBin), overlay);
  gtk_box_pack_end(GTK_BOX(root), widthBin, FALSE, TRUE, 0);
  return {
      .root = root,
      .levelBar = levelBar,
      .valueLabel = valueLabel,
  };
}

void updateStatusLevelMeter(const StatusLevelMeterWidgets &widgets,
                            const StatusLevelMeterState &state) {
  if (widgets.levelBar == nullptr || widgets.valueLabel == nullptr) {
    return;
  }

  const auto validRange =
      std::isfinite(state.minimum) && std::isfinite(state.maximum) &&
      state.maximum > state.minimum;
  const auto minimum = validRange ? state.minimum : 0.0;
  const auto maximum = validRange ? state.maximum : 1.0;
  const auto finiteValue =
      validRange && std::isfinite(state.value) ? state.value : minimum;
  const auto value = std::clamp(finiteValue, minimum, maximum);
  auto *levelBar = GTK_LEVEL_BAR(widgets.levelBar);
  gtk_level_bar_set_min_value(levelBar, minimum);
  gtk_level_bar_set_max_value(levelBar, maximum);
  gtk_level_bar_set_value(levelBar, value);
  setHueStep(widgets.levelBar, state.hueStep);

  const auto valueText = std::string(state.valueText);
  gtk_label_set_text(GTK_LABEL(widgets.valueLabel), valueText.c_str());
  auto *accessible = gtk_widget_get_accessible(widgets.levelBar);
  const auto accessibleName = std::string(state.accessibleName);
  const auto accessibleDescription =
      std::string(state.accessibleDescription);
  atk_object_set_name(accessible, accessibleName.c_str());
  atk_object_set_description(accessible,
                             accessibleDescription.c_str());
}

} // namespace pipetune_gtk
