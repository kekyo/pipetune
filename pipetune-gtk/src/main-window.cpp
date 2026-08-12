#include "main-window.h"

#include "gtk-resources.h"
#include "localization.h"

#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
#include <gestament/gtk.h>
#endif

#include <gdk/gdkx.h>

#include <optional>
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

static void configureCompactComboBox(GtkWidget *widget) {
  auto *comboBox = GTK_COMBO_BOX(widget);
  gtk_combo_box_set_popup_fixed_width(comboBox, FALSE);
  auto *cells = gtk_cell_layout_get_cells(GTK_CELL_LAYOUT(comboBox));
  for (auto *item = cells; item != nullptr; item = item->next) {
    if (GTK_IS_CELL_RENDERER_TEXT(item->data)) {
      g_object_set(G_OBJECT(item->data), "ellipsize", PANGO_ELLIPSIZE_END,
                   nullptr);
    }
  }
  g_list_free(cells);
}

MainWindowUi createMainWindowUi(GtkApplication *application,
                                std::string_view pipeTuneVersion,
                                std::string_view effetuneDspVersion) {
  ensureGtkResourcesRegistered();
  installStyle();
  auto *builder = gtk_builder_new();
  gtk_builder_set_translation_domain(builder, translationDomain());
  auto *builderError = static_cast<GError *>(nullptr);
  if (gtk_builder_add_from_resource(
          builder, kMainWindowResourcePath, &builderError) == 0) {
    g_error("Cannot load the PipeTune GTK main window: %s",
            builderError == nullptr ? "unknown error"
                                    : builderError->message);
  }
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
  gestament_gtk_assign_accessible_ids_from_builder(builder);
  auto *statusScrolledWindow = requiredWidget(
      builder, "statusScrolledWindow", GTK_TYPE_SCROLLED_WINDOW);
  gestament_gtk_assign_accessible_id(
      gtk_scrolled_window_get_vscrollbar(
          GTK_SCROLLED_WINDOW(statusScrolledWindow)),
      "statusScrollbar");
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
      .mainPaned =
          requiredWidget(builder, "mainPaned", GTK_TYPE_PANED),
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
      .statusLoadMeterBox = requiredWidget(
          builder, "statusLoadMeterBox", GTK_TYPE_BOX),
      .statusList =
          requiredWidget(builder, "statusList", GTK_TYPE_LIST_BOX),
      .processingEnabledSwitch = requiredWidget(
          builder, "processingEnabledSwitch", GTK_TYPE_SWITCH),
      .presetCombo =
          requiredWidget(builder, "presetCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .presetChooser = requiredWidget(
          builder, "presetChooser", GTK_TYPE_FILE_CHOOSER_BUTTON),
      .rateCombo =
          requiredWidget(builder, "rateCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .rateEnforcementCombo = requiredWidget(
          builder, "rateEnforcementCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .dspBackendCombo = requiredWidget(
          builder, "dspBackendCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .languageCombo = requiredWidget(
          builder, "languageCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .languageRestartNotice = requiredWidget(
          builder, "languageRestartNotice", GTK_TYPE_LABEL),
      .restoreDefaultsButton = requiredWidget(
          builder, "restoreDefaultsButton", GTK_TYPE_BUTTON),
      .pipeTuneVersionLink = requiredWidget(
          builder, "pipeTuneVersionLink", GTK_TYPE_LINK_BUTTON),
      .effetuneVersionLink = requiredWidget(
          builder, "effetuneVersionLink", GTK_TYPE_LINK_BUTTON),
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
  configureCompactComboBox(ui.presetCombo);
  configureCompactComboBox(ui.rateCombo);
  configureCompactComboBox(ui.rateEnforcementCombo);
  configureCompactComboBox(ui.dspBackendCombo);
  configureCompactComboBox(ui.languageCombo);
  auto *headerBar =
      requiredWidget(builder, "headerBar", GTK_TYPE_HEADER_BAR);
  const auto pipeTuneVersionText =
      "PipeTune " + std::string(pipeTuneVersion);
  const auto effetuneVersionText =
      "EffeTune DSP " + std::string(effetuneDspVersion);
  gtk_window_set_application(GTK_WINDOW(ui.window), application);
  gtk_window_set_title(GTK_WINDOW(ui.window), "PipeTune");
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerBar), "PipeTune");
  gtk_button_set_label(GTK_BUTTON(ui.pipeTuneVersionLink),
                       pipeTuneVersionText.c_str());
  gtk_button_set_label(GTK_BUTTON(ui.effetuneVersionLink),
                       effetuneVersionText.c_str());
  gtk_combo_box_set_active(GTK_COMBO_BOX(ui.logFilterCombo), 0);
  gtk_window_set_default(GTK_WINDOW(ui.window), ui.applyButton);

  auto geometry = GdkGeometry{};
  geometry.min_width = 900;
  geometry.min_height = 560;
  gtk_window_set_geometry_hints(
      GTK_WINDOW(ui.window), nullptr, &geometry, GDK_HINT_MIN_SIZE);
  gtk_widget_set_size_request(ui.window, 900, 560);

  auto *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, translate("EffeTune presets"));
  gtk_file_filter_add_pattern(filter, "*.effetune_preset");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(ui.presetChooser), filter);
  return ui;
}

std::optional<guint32> mainWindowPresentationTime(
    GtkWidget *window,
    std::optional<guint32> userInteractionTime) noexcept {
  if (userInteractionTime.has_value() &&
      userInteractionTime.value() != GDK_CURRENT_TIME) {
    return userInteractionTime;
  }
  if (window == nullptr || !gtk_widget_get_realized(window)) {
    return std::nullopt;
  }
  auto *gdkWindow = gtk_widget_get_window(window);
  if (gdkWindow == nullptr || !GDK_IS_X11_WINDOW(gdkWindow)) {
    return std::nullopt;
  }
  const auto events = gdk_window_get_events(gdkWindow);
  if ((events & GDK_PROPERTY_CHANGE_MASK) == 0) {
    gdk_window_set_events(
        gdkWindow,
        static_cast<GdkEventMask>(events | GDK_PROPERTY_CHANGE_MASK));
  }
  const auto serverTime = gdk_x11_get_server_time(gdkWindow);
  return serverTime == GDK_CURRENT_TIME
             ? std::optional<guint32>{}
             : std::optional<guint32>{serverTime};
}

void presentMainWindow(
    const MainWindowUi &ui,
    std::optional<guint32> userInteractionTime) noexcept {
  if (ui.window == nullptr) {
    return;
  }
  gtk_widget_show_all(ui.window);
  setLogDrawerVisible(
      ui, gtk_toggle_button_get_active(
              GTK_TOGGLE_BUTTON(ui.logToggleButton)) != FALSE);
  const auto presentationTime =
      mainWindowPresentationTime(ui.window, userInteractionTime);
  if (presentationTime.has_value()) {
    gtk_window_present_with_time(GTK_WINDOW(ui.window),
                                 presentationTime.value());
  } else {
    gtk_window_present(GTK_WINDOW(ui.window));
  }
}

void setLogDrawerVisible(const MainWindowUi &ui, bool visible) noexcept {
  if (ui.logRevealer == nullptr) {
    return;
  }
  if (visible) {
    gtk_widget_show(ui.logRevealer);
    gtk_revealer_set_reveal_child(GTK_REVEALER(ui.logRevealer), TRUE);
    return;
  }
  gtk_revealer_set_reveal_child(GTK_REVEALER(ui.logRevealer), FALSE);
  gtk_widget_hide(ui.logRevealer);
}

MainWindowViewState
captureMainWindowViewState(const MainWindowUi &ui) noexcept {
  auto width = int{};
  auto height = int{};
  auto x = int{};
  auto y = int{};
  gtk_window_get_size(GTK_WINDOW(ui.window), &width, &height);
  gtk_window_get_position(GTK_WINDOW(ui.window), &x, &y);
  const auto *page = gtk_stack_get_visible_child_name(
      GTK_STACK(ui.settingsStack));
  return {
      .settingsPage = page == nullptr ? std::string{}
                                     : std::string(page),
      .mainPanedPosition =
          gtk_paned_get_position(GTK_PANED(ui.mainPaned)),
      .logVisible =
          gtk_toggle_button_get_active(
              GTK_TOGGLE_BUTTON(ui.logToggleButton)) != FALSE,
      .logFilter =
          gtk_combo_box_get_active(GTK_COMBO_BOX(ui.logFilterCombo)),
      .windowWidth = width,
      .windowHeight = height,
      .windowX = x,
      .windowY = y,
      .maximized =
          gtk_window_is_maximized(GTK_WINDOW(ui.window)) != FALSE,
  };
}

void restoreMainWindowViewState(
    const MainWindowUi &ui,
    const MainWindowViewState &state) noexcept {
  auto *settingsPage =
      state.settingsPage.empty()
          ? nullptr
          : gtk_stack_get_child_by_name(
                GTK_STACK(ui.settingsStack),
                state.settingsPage.c_str());
  if (settingsPage != nullptr) {
    gtk_widget_show(settingsPage);
    gtk_stack_set_visible_child_name(GTK_STACK(ui.settingsStack),
                                     state.settingsPage.c_str());
  }
  gtk_paned_set_position(GTK_PANED(ui.mainPaned),
                         state.mainPanedPosition);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.logToggleButton),
                               state.logVisible ? TRUE : FALSE);
  setLogDrawerVisible(ui, state.logVisible);
  gtk_combo_box_set_active(GTK_COMBO_BOX(ui.logFilterCombo),
                           state.logFilter);
  if (state.windowWidth > 0 && state.windowHeight > 0) {
    gtk_window_resize(GTK_WINDOW(ui.window), state.windowWidth,
                      state.windowHeight);
  }
  gtk_window_move(GTK_WINDOW(ui.window), state.windowX, state.windowY);
  if (state.maximized) {
    gtk_window_maximize(GTK_WINDOW(ui.window));
  } else {
    gtk_window_unmaximize(GTK_WINDOW(ui.window));
  }
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
