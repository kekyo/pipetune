/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "main-window.h"

#include "gtk-resources.h"
#include "localization.h"
#include "output-mapping-model.h"

#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
#include <gestament/gtk.h>
#endif

#include <gdk/gdkx.h>

#include <optional>
#include <algorithm>
#include <array>
#include <string>

namespace pipetune_gtk {

void renderOutputLevels(MainWindowUi &ui,
    const pipetune::OutputConfiguration &configuration,
    std::span<const float> peaks, bool connected) {
  const auto single = configuration.mode == pipetune::OutputMode::single;
  const auto count = std::min<std::size_t>(16,
      peaks.empty() ? ui.outputLevelRows.size() : peaks.size());
  if (ui.outputLevelRows.size() != count) {
    for (const auto &row : ui.outputLevelRows) gtk_widget_destroy(row.root);
    ui.outputLevelRows.clear();
    for (auto index = std::size_t{0}; index < count; ++index) {
      auto row = createOutputLevelMeter();
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
      gestament_gtk_assign_accessible_id(row.levelBar, ("output-level-" + std::to_string(index + 1)).c_str());
#endif
      gtk_box_pack_start(GTK_BOX(ui.outputLevelMeters), row.root, FALSE, FALSE, 0);
      ui.outputLevelRows.push_back(row);
    }
    gtk_widget_show_all(ui.outputLevelMeters);
  }
  for (auto index = std::size_t{0}; index < count; ++index) {
    const auto purpose = !single && index < configuration.channels.size()
        ? std::string_view(configuration.channels[index].label) : std::string_view{};
    updateOutputLevelMeter(ui.outputLevelRows[index], "Ch " + std::to_string(index + 1), purpose,
        connected && index < peaks.size() ? std::optional<float>{peaks[index]} : std::nullopt);
  }
}

static void configureOutputOverview(GtkWidget *widget, bool selectable) {
  auto *view = GTK_TREE_VIEW(widget);
  auto *store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING,
      G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
  gtk_tree_view_set_model(view, GTK_TREE_MODEL(store));
  g_object_unref(store);
  const auto headings = std::array{translate("Device name"), translate("State"), translate("Channels")};
  const auto widths = std::array{150, 125, 130};
  for (auto index = 0; index < 3; ++index) {
    auto *cell = gtk_cell_renderer_text_new();
    g_object_set(cell, "ypad", 6, "xpad", 8, nullptr);
    if (index == 0) g_object_set(cell, "ellipsize", PANGO_ELLIPSIZE_END, nullptr);
    else g_object_set(cell, "wrap-mode", PANGO_WRAP_WORD_CHAR,
        "wrap-width", widths[index] - 16, nullptr);
    auto *column = gtk_tree_view_column_new_with_attributes(headings[index], cell, "text", index, nullptr);
    gtk_tree_view_column_set_sizing(column, GTK_TREE_VIEW_COLUMN_FIXED);
    gtk_tree_view_column_set_fixed_width(column, widths[index]);
    gtk_tree_view_column_set_expand(column, index == 0);
    gtk_tree_view_append_column(view, column);
  }
  gtk_tree_view_set_tooltip_column(view, 4);
  gtk_tree_selection_set_mode(gtk_tree_view_get_selection(view), selectable ? GTK_SELECTION_SINGLE : GTK_SELECTION_NONE);
}

void renderOutputOverview(GtkWidget *widget,
    const pipetune::OutputConfiguration &configuration,
    std::span<const pipetune::AvailableOutput> inventory,
    bool inventoryReady, std::string_view inventoryError) {
  auto enabled = configuration;
  for (auto &output : enabled.outputs) output.enabled = true;
  const auto resolved = pipetune::resolveConfiguredOutputs(enabled, inventory);
  auto rows = std::vector<std::array<std::string, 5>>{};
  for (const auto index : configuredOutputOrder(configuration)) {
    const auto &output = configuration.outputs[index];
    auto state = std::string{};
    if (!inventoryError.empty()) state = translate("Inventory unavailable");
    else if (!inventoryReady) state = translate("Waiting for output devices…");
    else switch (resolved[index].state) {
    case pipetune::OutputConnectionState::connected: state = translate("Connected"); break;
    case pipetune::OutputConnectionState::missing: state = translate("Not connected"); break;
    case pipetune::OutputConnectionState::profileMismatch: state = translate("Profile changed"); break;
    case pipetune::OutputConnectionState::ambiguous: state = translate("Ambiguous device"); break;
    case pipetune::OutputConnectionState::disabled: break;
    }
    if (!output.enabled) state = std::string(translate("Disabled")) + " · " + state;
    auto channels = std::string{};
    for (auto slot = std::size_t{0}; slot < configuration.channels.size(); ++slot) {
      if (configuration.channels[slot].outputId != output.id) continue;
      if (!channels.empty()) channels += ", ";
      channels += "Ch " + std::to_string(slot + 1);
    }
    if (channels.empty()) channels = translate("Unassigned");
    rows.push_back({output.device.name, state, channels, output.id,
        output.device.name + "\n" + translate("Output profile") + ": " + output.device.profile});
  }
  auto *view = GTK_TREE_VIEW(widget);
  auto *model = gtk_tree_view_get_model(view);
  auto same = gtk_tree_model_iter_n_children(model, nullptr) == static_cast<int>(rows.size());
  auto iter = GtkTreeIter{};
  for (auto index = std::size_t{0}; same && index < rows.size(); ++index) {
    gtk_tree_model_iter_nth_child(model, &iter, nullptr, static_cast<int>(index));
    for (auto column = 0; column < 5; ++column) {
      auto *text = static_cast<gchar *>(nullptr);
      gtk_tree_model_get(model, &iter, column, &text, -1);
      same = same && text != nullptr && rows[index][column] == text;
      g_free(text);
    }
  }
  if (same) return;
  auto *selection = gtk_tree_view_get_selection(view);
  auto *selectedId = static_cast<gchar *>(nullptr);
  if (gtk_tree_selection_get_selected(selection, nullptr, &iter))
    gtk_tree_model_get(model, &iter, 3, &selectedId, -1);
  auto *store = GTK_LIST_STORE(model);
  gtk_list_store_clear(store);
  for (const auto &row : rows) {
    gtk_list_store_insert_with_values(store, &iter, -1, 0, row[0].c_str(),
        1, row[1].c_str(), 2, row[2].c_str(), 3, row[3].c_str(), 4, row[4].c_str(), -1);
    if (selectedId != nullptr && row[3] == selectedId) gtk_tree_selection_select_iter(selection, &iter);
  }
  g_free(selectedId);
}

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

static const char *presetEntryStateText(pipetune::PresetEntryState state) {
  switch (state) {
  case pipetune::PresetEntryState::enabled:
    return translate("Enabled");
  case pipetune::PresetEntryState::off:
    return translate("Off");
  case pipetune::PresetEntryState::ignored:
    return translate("Ignored");
  }
  return "";
}

void renderPresetConfiguration(
    MainWindowUi &ui,
    const std::optional<std::vector<pipetune::PresetEntry>> &entries) {
  if (ui.displayedPresetEntries == entries) {
    return;
  }
  auto *model = GTK_LIST_STORE(
      gtk_tree_view_get_model(GTK_TREE_VIEW(ui.presetEntryView)));
  gtk_list_store_clear(model);
  if (entries.has_value()) {
    for (const auto &entry : *entries) {
      auto state = std::string(presetEntryStateText(entry.state));
      for (const auto &diagnostic : entry.diagnostics) state += "\n" + diagnostic;
      gtk_list_store_insert_with_values(
          model, nullptr, -1, 0, entry.name.c_str(),
          1, state.c_str(), -1);
    }
  }
  gtk_widget_set_visible(ui.presetConfigurationSection, entries.has_value());
  ui.displayedPresetEntries = entries;
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
  gestament_gtk_assign_accessible_id(
      gtk_scrolled_window_get_vscrollbar(GTK_SCROLLED_WINDOW(
          requiredWidget(builder, "outputScrolledWindow", GTK_TYPE_SCROLLED_WINDOW))),
      "outputScrollbar");
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
      .presetConfigurationSection = requiredWidget(
          builder, "presetConfigurationSection", GTK_TYPE_FRAME),
      .presetEntryView = requiredWidget(
          builder, "presetEntryView", GTK_TYPE_TREE_VIEW),
      .outputLevelMeters = requiredWidget(builder, "outputLevelMeters", GTK_TYPE_BOX),
      .outputModeCombo = requiredWidget(builder, "outputModeCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .outputModeNotice = requiredWidget(builder, "outputModeNotice", GTK_TYPE_LABEL),
      .outputErrorLabel = requiredWidget(builder, "outputErrorLabel", GTK_TYPE_LABEL),
      .outputDeviceView = requiredWidget(builder, "outputDeviceView", GTK_TYPE_TREE_VIEW),
      .outputDeviceDetails = requiredWidget(builder, "outputDeviceDetails", GTK_TYPE_STACK),
      .outputSelectedDeviceLabel = requiredWidget(builder, "outputSelectedDeviceLabel", GTK_TYPE_LABEL),
      .outputEmptyLabel = requiredWidget(builder, "outputEmptyLabel", GTK_TYPE_LABEL),
      .outputAddButton = requiredWidget(builder, "outputAddButton", GTK_TYPE_BUTTON),
      .outputDeleteButton = requiredWidget(builder, "outputDeleteButton", GTK_TYPE_BUTTON),
      .outputCompactButton = requiredWidget(builder, "outputCompactButton", GTK_TYPE_BUTTON),
      .outputReplaceButton = requiredWidget(builder, "outputReplaceButton", GTK_TYPE_BUTTON),
      .outputPickerBox = requiredWidget(builder, "outputPickerBox", GTK_TYPE_BOX),
      .outputPickerNotice = requiredWidget(builder, "outputPickerNotice", GTK_TYPE_LABEL),
      .outputAvailableCombo = requiredWidget(builder, "outputAvailableCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .outputChooseButton = requiredWidget(builder, "outputChooseButton", GTK_TYPE_BUTTON),
      .outputChooseCancelButton = requiredWidget(builder, "outputChooseCancelButton", GTK_TYPE_BUTTON),
      .outputChangeSummaryLabel = requiredWidget(builder, "outputChangeSummaryLabel", GTK_TYPE_LABEL),
      .outputReservedExpander = requiredWidget(builder, "outputReservedExpander", GTK_TYPE_EXPANDER),
      .outputReservedGrid = requiredWidget(builder, "outputReservedGrid", GTK_TYPE_GRID),
      .outputConfigureButton = requiredWidget(builder, "outputConfigureButton", GTK_TYPE_BUTTON),
      .outputSummaryView = requiredWidget(builder, "outputSummaryView", GTK_TYPE_TREE_VIEW),
      .outputConfigurationDialog = requiredWidget(builder, "outputConfigurationDialog", GTK_TYPE_DIALOG),
      .outputConfigurationApplyButton = requiredWidget(builder, "outputConfigurationApplyButton", GTK_TYPE_BUTTON),
      .outputConfigurationCancelButton = requiredWidget(builder, "outputConfigurationCancelButton", GTK_TYPE_BUTTON),
      .rateCombo =
          requiredWidget(builder, "rateCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .rateEnforcementCombo = requiredWidget(
          builder, "rateEnforcementCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .dspBackendCombo = requiredWidget(
          builder, "dspBackendCombo", GTK_TYPE_COMBO_BOX_TEXT),
      .dspIdleEnabledSwitch = requiredWidget(
          builder, "dspIdleEnabledSwitch", GTK_TYPE_SWITCH),
      .dspIdleTimeoutSpin = requiredWidget(
          builder, "dspIdleTimeoutSpin", GTK_TYPE_SPIN_BUTTON),
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
  configureCompactComboBox(ui.outputModeCombo);
  gtk_widget_show_all(
      gtk_bin_get_child(GTK_BIN(ui.presetConfigurationSection)));
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
  configureOutputOverview(ui.outputSummaryView, false);
  configureOutputOverview(ui.outputDeviceView, true);
  configureCompactComboBox(ui.outputAvailableCombo);

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
