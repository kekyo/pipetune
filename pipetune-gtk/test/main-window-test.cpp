#include "application-state.h"
#include "localization.h"
#include "main-window.h"
#include "status-model.h"

#include <gtk/gtk.h>

#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool checkWidgetTypes(const pipetune_gtk::MainWindowUi &ui) {
  return check(GTK_IS_APPLICATION_WINDOW(ui.window),
               "main window type differs") &&
         check(GTK_IS_BUTTON(ui.closeButton),
               "close button type differs") &&
         check(GTK_IS_BOX(ui.persistentStatusPane),
               "persistent status pane type differs") &&
         check(GTK_IS_BOX(ui.settingsPane),
               "settings pane type differs") &&
         check(GTK_IS_STACK_SWITCHER(ui.settingsSwitcher),
               "settings switcher type differs") &&
         check(GTK_IS_STACK(ui.settingsStack),
               "settings stack type differs") &&
         check(GTK_IS_IMAGE(ui.statusImage),
               "status image type differs") &&
         check(GTK_IS_IMAGE(ui.statusBadge),
               "status badge type differs") &&
         check(GTK_IS_LABEL(ui.connectionSummaryLabel),
               "connection summary type differs") &&
         check(GTK_IS_BOX(ui.statusLoadMeterBox),
               "status Load meter host type differs") &&
         check(GTK_IS_LIST_BOX(ui.statusList),
               "status list type differs") &&
         check(GTK_IS_SWITCH(ui.processingEnabledSwitch),
               "processing switch type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.presetCombo),
               "preset combo-box type differs") &&
         check(GTK_IS_FILE_CHOOSER_BUTTON(ui.presetChooser),
               "preset chooser type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.rateCombo),
               "rate combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.rateEnforcementCombo),
               "rate enforcement combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.dspBackendCombo),
               "DSP backend combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.languageCombo),
               "language combo-box type differs") &&
         check(GTK_IS_LABEL(ui.languageRestartNotice),
               "language restart notice type differs") &&
         check(GTK_IS_BUTTON(ui.restoreDefaultsButton),
               "restore-defaults button type differs") &&
         check(GTK_IS_LABEL(ui.versionLabel),
               "version label type differs") &&
         check(GTK_IS_TOGGLE_BUTTON(ui.logToggleButton),
               "log toggle type differs") &&
         check(GTK_IS_REVEALER(ui.logRevealer),
               "log revealer type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.logFilterCombo),
               "log filter type differs") &&
         check(GTK_IS_BUTTON(ui.logCopyButton),
               "log copy button type differs") &&
         check(GTK_IS_BUTTON(ui.logClearButton),
               "log clear button type differs") &&
         check(GTK_IS_LIST_BOX(ui.logList),
               "log list type differs") &&
         check(GTK_IS_LABEL(ui.transactionStateLabel),
               "transaction state type differs") &&
         check(GTK_IS_BUTTON(ui.cancelButton),
               "cancel button type differs") &&
         check(GTK_IS_BUTTON(ui.applyButton),
               "global apply button type differs");
}

static bool checkSettingsPages(const pipetune_gtk::MainWindowUi &ui) {
  auto *children =
      gtk_container_get_children(GTK_CONTAINER(ui.settingsStack));
  const auto count = g_list_length(children);
  g_list_free(children);
  return check(count == 4, "settings page count differs") &&
         check(gtk_stack_get_child_by_name(GTK_STACK(ui.settingsStack),
                                           "processing") != nullptr,
               "processing page is missing") &&
         check(gtk_stack_get_child_by_name(GTK_STACK(ui.settingsStack),
                                           "rate") != nullptr,
               "rate page is missing") &&
         check(gtk_stack_get_child_by_name(GTK_STACK(ui.settingsStack),
                                           "dsp") != nullptr,
               "DSP page is missing") &&
         check(gtk_stack_get_child_by_name(GTK_STACK(ui.settingsStack),
                                           "advanced") != nullptr,
               "advanced page is missing");
}

static bool checkLoadMeterHost(const pipetune_gtk::MainWindowUi &ui) {
  auto *heading = GTK_WIDGET(
      gtk_builder_get_object(ui.builder, "statusHeadingLabel"));
  auto *host = ui.statusLoadMeterBox;
  if (!check(GTK_IS_LABEL(heading),
             "status heading label is missing") ||
      !check(GTK_IS_BOX(host), "status Load meter host is missing")) {
    return false;
  }
  auto *labelColumn = gtk_widget_get_parent(ui.connectionSummaryLabel);
  if (!check(labelColumn != nullptr && GTK_IS_BOX(labelColumn),
             "connection label column is missing") ||
      !check(gtk_widget_get_parent(heading) == labelColumn &&
                 gtk_widget_get_parent(host) == labelColumn,
             "status Load meter must share the connection label column")) {
    return false;
  }
  auto *children = gtk_container_get_children(GTK_CONTAINER(labelColumn));
  const auto headingPosition = g_list_index(children, heading);
  const auto summaryPosition =
      g_list_index(children, ui.connectionSummaryLabel);
  const auto hostPosition = g_list_index(children, host);
  g_list_free(children);
  return check(headingPosition >= 0 &&
                   summaryPosition > headingPosition &&
                   hostPosition > summaryPosition,
               "status Load meter must follow the connection summary");
}

static bool checkJapaneseMeasuredStatusLabels() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.dspTiming.hasAverage = true;
  state.dspTiming.nanosecondsPerFrame = 100.0;
  state.dspTiming.loadPercent = 1.0;
  const auto sections = pipetune_gtk::buildStatusSections(
      state, pipetune::StartupConfig{}, 0);
  for (const auto &section : sections) {
    for (const auto &item : section.items) {
      if (item.id == "dsp.load") {
        return check(item.label == "負荷",
                     "measured DSP load label was not translated");
      }
    }
  }
  return check(false, "measured DSP load status is missing");
}

int main(int argc, char **argv) {
  if (argc != 2) {
    std::cerr << "locale directory is required\n";
    return 1;
  }
  const auto localeDirectory = std::filesystem::path(argv[1]);
  const auto originalLocalization =
      pipetune_gtk::captureUiLocalizationEnvironment();
  const auto english = pipetune_gtk::applyUiLanguage(
      originalLocalization, pipetune_gtk::UiLanguage::english,
      localeDirectory);
  if (!english.warning.empty()) {
    std::cerr << english.warning << '\n';
    return 1;
  }
  gtk_disable_setlocale();
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "GTK display is unavailable\n";
    return 1;
  }
  auto *application = gtk_application_new(
      "net.kekyo.pipetune-gtk.tests", G_APPLICATION_NON_UNIQUE);
  auto *registrationError = static_cast<GError *>(nullptr);
  if (!g_application_register(G_APPLICATION(application), nullptr,
                              &registrationError)) {
    std::cerr << "GTK application registration failed: "
              << (registrationError == nullptr
                      ? "unknown error"
                      : registrationError->message)
              << '\n';
    g_clear_error(&registrationError);
    g_object_unref(application);
    return 1;
  }
  auto ui = pipetune_gtk::createMainWindowUi(
      application, "1.2.3", "4.5.6");

  auto width = int{0};
  auto height = int{0};
  gtk_window_get_default_size(GTK_WINDOW(ui.window), &width, &height);
  auto minimumWidth = int{0};
  auto minimumHeight = int{0};
  gtk_widget_get_size_request(ui.window, &minimumWidth, &minimumHeight);
  auto *headerBar = gtk_builder_get_object(ui.builder, "headerBar");
  const auto valid =
      check(ui.builder != nullptr, "main window builder is unavailable") &&
      checkWidgetTypes(ui) && checkSettingsPages(ui) &&
      checkLoadMeterHost(ui) &&
      check(gtk_window_get_application(GTK_WINDOW(ui.window)) ==
                application,
            "main window application differs") &&
      check(std::string_view(
                gtk_window_get_title(GTK_WINDOW(ui.window))) ==
                "PipeTune",
            "main window title differs") &&
      check(GTK_IS_HEADER_BAR(headerBar),
            "header bar type differs") &&
      check(std::string_view(gtk_header_bar_get_title(
                GTK_HEADER_BAR(headerBar))) == "PipeTune",
            "header bar title differs") &&
      check(width == 1080 && height == 680,
            "main window default size differs") &&
      check(minimumWidth == 900 && minimumHeight == 560,
            "main window minimum size differs") &&
      check(gtk_stack_switcher_get_stack(
                GTK_STACK_SWITCHER(ui.settingsSwitcher)) ==
                GTK_STACK(ui.settingsStack),
            "settings switcher is not attached to the stack") &&
      check(gtk_box_get_homogeneous(
                GTK_BOX(ui.settingsSwitcher)) == FALSE,
            "settings switcher must fit labels independently") &&
      check(gtk_revealer_get_reveal_child(
                GTK_REVEALER(ui.logRevealer)) == FALSE,
            "log drawer must start collapsed") &&
      check(gtk_widget_get_visible(ui.languageRestartNotice) == FALSE,
            "language restart notice must start hidden") &&
      check(std::string_view(
                gtk_label_get_text(GTK_LABEL(ui.versionLabel))) ==
                "PipeTune 1.2.3  •  EffeTune DSP 4.5.6",
            "version text differs") &&
      check(gtk_builder_get_object(ui.builder, "rateApplyButton") == nullptr,
            "per-setting rate Apply button must not exist") &&
      check(gtk_builder_get_object(ui.builder,
                                   "dspBackendApplyButton") == nullptr,
            "per-setting backend Apply button must not exist");

  gtk_widget_hide(ui.window);
  pipetune_gtk::presentMainWindow(ui, 1234);
  const auto presentationWorks =
      check(gtk_widget_get_visible(ui.window) != FALSE,
            "presenting the main window must make it visible") &&
      check(gtk_widget_get_visible(ui.persistentStatusPane) != FALSE,
            "persistent status pane must be visible") &&
      check(gtk_widget_get_visible(ui.settingsPane) != FALSE,
            "settings pane must be visible") &&
      check(gtk_widget_get_visible(ui.logRevealer) == FALSE,
            "collapsed log drawer must not retain window height");

  gtk_stack_set_transition_duration(GTK_STACK(ui.settingsStack), 0);
  gtk_stack_set_visible_child_name(GTK_STACK(ui.settingsStack),
                                   "advanced");
  gtk_paned_set_position(GTK_PANED(ui.mainPaned), 455);
  gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(ui.logToggleButton),
                               TRUE);
  gtk_revealer_set_transition_duration(GTK_REVEALER(ui.logRevealer), 0);
  gtk_combo_box_set_active(GTK_COMBO_BOX(ui.logFilterCombo), 2);
  pipetune_gtk::setLogDrawerVisible(ui, true);
  const auto viewState =
      pipetune_gtk::captureMainWindowViewState(ui);
  pipetune_gtk::destroyMainWindowUi(ui);
  while (gtk_events_pending() != FALSE) {
    gtk_main_iteration();
  }
  ui = pipetune_gtk::createMainWindowUi(
      application, "1.2.3", "4.5.6");
  gtk_stack_set_transition_duration(GTK_STACK(ui.settingsStack), 0);
  gtk_revealer_set_transition_duration(GTK_REVEALER(ui.logRevealer), 0);
  pipetune_gtk::restoreMainWindowViewState(ui, viewState);
  const auto *restoredPage = gtk_stack_get_visible_child_name(
      GTK_STACK(ui.settingsStack));
  const auto rebuildPreservesView =
      check(restoredPage != nullptr &&
                std::string_view(restoredPage) == "advanced",
            "a presentation rebuild must retain the selected page") &&
      check(gtk_paned_get_position(GTK_PANED(ui.mainPaned)) == 455,
            "a presentation rebuild must retain the pane position") &&
      check(gtk_toggle_button_get_active(
                GTK_TOGGLE_BUTTON(ui.logToggleButton)) != FALSE &&
                gtk_widget_get_visible(ui.logRevealer) != FALSE,
            "a presentation rebuild must retain log visibility") &&
      check(gtk_combo_box_get_active(
                GTK_COMBO_BOX(ui.logFilterCombo)) == 2,
            "a presentation rebuild must retain the log filter");
  pipetune_gtk::destroyMainWindowUi(ui);

  const auto japanese = pipetune_gtk::applyUiLanguage(
      originalLocalization, pipetune_gtk::UiLanguage::japanese,
      localeDirectory);
  auto japaneseUi = pipetune_gtk::createMainWindowUi(
      application, "1.2.3", "4.5.6");
  const auto japanesePresentationWorks =
      check(japanese.warning.empty(),
            "Japanese localization could not be applied") &&
      check(std::string_view(gtk_button_get_label(
                GTK_BUTTON(japaneseUi.restoreDefaultsButton))) ==
                "既定値に戻す",
            "GtkBuilder button was not translated") &&
      check(std::string_view(
                pipetune_gtk::translate("Connected")) == "接続済み",
            "dynamic presentation text was not translated") &&
      checkJapaneseMeasuredStatusLabels();
  pipetune_gtk::destroyMainWindowUi(japaneseUi);
  pipetune_gtk::restoreUiLocalizationEnvironment(
      originalLocalization);
  g_object_unref(application);
  return valid && presentationWorks && rebuildPreservesView &&
                 japanesePresentationWorks
             ? 0
             : 1;
}
