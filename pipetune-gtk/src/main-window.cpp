#include "main-window.h"

#include "gtk-resources.h"

#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
#include <gestament/gtk.h>
#endif

#include <string>

namespace pipetune_gtk {

constexpr char kMainWindowResourcePath[] =
    "/net/kekyo/pipetune-gtk/ui/main-window.ui";
constexpr char kStyleResourcePath[] =
    "/net/kekyo/pipetune-gtk/ui/style.css";

static GtkWidget *requiredWidget(GtkBuilder *builder, const char *name,
                                 GType expectedType) {
  auto *object = gtk_builder_get_object(builder, name);
  if (object == nullptr ||
      !g_type_is_a(G_OBJECT_TYPE(object), expectedType)) {
    g_error("PipeTune GTK UI object '%s' is missing or has the wrong type",
            name);
  }
  return GTK_WIDGET(object);
}

static void installStyle() {
  auto *provider = gtk_css_provider_new();
  gtk_css_provider_load_from_resource(provider, kStyleResourcePath);
  auto *screen = gdk_screen_get_default();
  if (screen != nullptr) {
    gtk_style_context_add_provider_for_screen(
        screen, GTK_STYLE_PROVIDER(provider),
        GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
  }
  g_object_unref(provider);
}

MainWindowUi createMainWindowUi(GtkApplication *application,
                                std::string_view pipeTuneVersion,
                                std::string_view effetuneDspVersion) {
  ensureGtkResourcesRegistered();
  installStyle();
  auto *builder = gtk_builder_new_from_resource(kMainWindowResourcePath);
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
  gestament_gtk_assign_accessible_ids_from_builder(builder);
#endif
  auto ui = MainWindowUi{
      .builder = builder,
      .window = requiredWidget(builder, "mainWindow",
                               GTK_TYPE_APPLICATION_WINDOW),
      .closeButton =
          requiredWidget(builder, "closeButton", GTK_TYPE_BUTTON),
      .persistentStatusPane =
          requiredWidget(builder, "persistentStatusPane", GTK_TYPE_BOX),
      .settingsPane =
          requiredWidget(builder, "settingsPane", GTK_TYPE_BOX),
      .settingsSwitcher = requiredWidget(
          builder, "settingsSwitcher", GTK_TYPE_STACK_SWITCHER),
      .settingsStack =
          requiredWidget(builder, "settingsStack", GTK_TYPE_STACK),
      .statusImage =
          requiredWidget(builder, "statusImage", GTK_TYPE_IMAGE),
      .statusBadge =
          requiredWidget(builder, "statusBadge", GTK_TYPE_IMAGE),
      .connectionSummaryLabel = requiredWidget(
          builder, "connectionSummaryLabel", GTK_TYPE_LABEL),
      .statusList =
          requiredWidget(builder, "statusList", GTK_TYPE_LIST_BOX),
      .processingEnabledSwitch = requiredWidget(
          builder, "processingEnabledSwitch", GTK_TYPE_SWITCH),
      .presetCombo =
          requiredWidget(builder, "presetCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .presetChooser = requiredWidget(
          builder, "presetChooser", GTK_TYPE_FILE_CHOOSER_BUTTON),
      .outputMenuButton = requiredWidget(
          builder, "outputMenuButton", GTK_TYPE_MENU_BUTTON),
      .outputButtonLabel =
          requiredWidget(builder, "outputButtonLabel", GTK_TYPE_LABEL),
      .outputPopover =
          requiredWidget(builder, "outputPopover", GTK_TYPE_POPOVER),
      .outputList =
          requiredWidget(builder, "outputList", GTK_TYPE_LIST_BOX),
      .rateCombo =
          requiredWidget(builder, "rateCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .rateEnforcementCombo = requiredWidget(
          builder, "rateEnforcementCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .dspBackendCombo = requiredWidget(
          builder, "dspBackendCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .dspIdlePolicyCombo = requiredWidget(
          builder, "dspIdlePolicyCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .restoreDefaultsButton = requiredWidget(
          builder, "restoreDefaultsButton", GTK_TYPE_BUTTON),
      .versionLabel =
          requiredWidget(builder, "versionLabel", GTK_TYPE_LABEL),
      .logToggleButton = requiredWidget(
          builder, "logToggleButton", GTK_TYPE_TOGGLE_BUTTON),
      .logToggleLabel =
          requiredWidget(builder, "logToggleLabel", GTK_TYPE_LABEL),
      .logRevealer =
          requiredWidget(builder, "logRevealer", GTK_TYPE_REVEALER),
      .logFilterCombo = requiredWidget(
          builder, "logFilterCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .logCopyButton =
          requiredWidget(builder, "logCopyButton", GTK_TYPE_BUTTON),
      .logClearButton =
          requiredWidget(builder, "logClearButton", GTK_TYPE_BUTTON),
      .logList =
          requiredWidget(builder, "logList", GTK_TYPE_LIST_BOX),
      .transactionStateLabel = requiredWidget(
          builder, "transactionStateLabel", GTK_TYPE_LABEL),
      .cancelButton =
          requiredWidget(builder, "cancelButton", GTK_TYPE_BUTTON),
      .applyButton =
          requiredWidget(builder, "applyButton", GTK_TYPE_BUTTON),
  };
  auto *headerBar =
      requiredWidget(builder, "headerBar", GTK_TYPE_HEADER_BAR);
  const auto versionText =
      "PipeTune " + std::string(pipeTuneVersion) +
      "  •  EffeTune DSP " + std::string(effetuneDspVersion);
  gtk_window_set_application(GTK_WINDOW(ui.window), application);
  gtk_window_set_title(GTK_WINDOW(ui.window), "PipeTune");
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerBar), "PipeTune");
  gtk_label_set_text(GTK_LABEL(ui.versionLabel), versionText.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(ui.logFilterCombo), 0);
  gtk_window_set_default(GTK_WINDOW(ui.window), ui.applyButton);

  auto geometry = GdkGeometry{};
  geometry.min_width = 900;
  geometry.min_height = 560;
  gtk_window_set_geometry_hints(
      GTK_WINDOW(ui.window), nullptr, &geometry, GDK_HINT_MIN_SIZE);
  gtk_widget_set_size_request(ui.window, 900, 560);

  auto *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "EffeTune presets");
  gtk_file_filter_add_pattern(filter, "*.effetune_preset");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(ui.presetChooser), filter);
  return ui;
}

void presentMainWindow(const MainWindowUi &ui,
                       guint32 userInteractionTime) noexcept {
  if (ui.window == nullptr) {
    return;
  }
  gtk_widget_show_all(ui.window);
  gtk_revealer_set_reveal_child(GTK_REVEALER(ui.logRevealer),
                                gtk_toggle_button_get_active(
                                    GTK_TOGGLE_BUTTON(ui.logToggleButton)));
  gtk_window_present_with_time(GTK_WINDOW(ui.window),
                               userInteractionTime);
}

void destroyMainWindowUi(MainWindowUi &ui) noexcept {
  if (ui.window != nullptr) {
    gtk_widget_destroy(ui.window);
  }
  if (ui.builder != nullptr) {
    g_object_unref(ui.builder);
  }
  ui = {};
}

} // namespace pipetune_gtk
