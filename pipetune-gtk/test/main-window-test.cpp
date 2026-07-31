#include "main-window.h"

#include <gtk/gtk.h>

#include <iostream>
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
         check(GTK_IS_LIST_BOX(ui.statusList),
               "status list type differs") &&
         check(GTK_IS_SWITCH(ui.processingEnabledSwitch),
               "processing switch type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.presetCombo),
               "preset combo-box type differs") &&
         check(GTK_IS_FILE_CHOOSER_BUTTON(ui.presetChooser),
               "preset chooser type differs") &&
         check(GTK_IS_MENU_BUTTON(ui.outputMenuButton),
               "output menu button type differs") &&
         check(GTK_IS_LABEL(ui.outputButtonLabel),
               "output button label type differs") &&
         check(GTK_IS_POPOVER(ui.outputPopover),
               "output popover type differs") &&
         check(GTK_IS_LIST_BOX(ui.outputList),
               "output list type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.rateCombo),
               "rate combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.rateEnforcementCombo),
               "rate enforcement combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.dspBackendCombo),
               "DSP backend combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.dspIdlePolicyCombo),
               "DSP idle policy combo-box type differs") &&
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
  return check(count == 5, "settings page count differs") &&
         check(gtk_stack_get_child_by_name(GTK_STACK(ui.settingsStack),
                                           "processing") != nullptr,
               "processing page is missing") &&
         check(gtk_stack_get_child_by_name(GTK_STACK(ui.settingsStack),
                                           "output") != nullptr,
               "output page is missing") &&
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

int main(int argc, char **argv) {
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "GTK display is unavailable\n";
    return 1;
  }
  auto *application = gtk_application_new(
      "net.kekyo.pipetune-gtk.tests", G_APPLICATION_NON_UNIQUE);
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
      check(gtk_revealer_get_reveal_child(
                GTK_REVEALER(ui.logRevealer)) == FALSE,
            "log drawer must start collapsed") &&
      check(std::string_view(
                gtk_label_get_text(GTK_LABEL(ui.versionLabel))) ==
                "PipeTune 1.2.3  •  EffeTune DSP 4.5.6",
            "version text differs") &&
      check(gtk_builder_get_object(ui.builder, "rateApplyButton") == nullptr,
            "per-setting rate Apply button must not exist") &&
      check(gtk_builder_get_object(ui.builder,
                                   "dspBackendApplyButton") == nullptr,
            "per-setting backend Apply button must not exist") &&
      check(gtk_builder_get_object(ui.builder,
                                   "dspIdlePolicyApplyButton") == nullptr,
            "per-setting idle Apply button must not exist");

  gtk_widget_hide(ui.window);
  pipetune_gtk::presentMainWindow(ui, 1234);
  const auto presentationWorks =
      check(gtk_widget_get_visible(ui.window) != FALSE,
            "presenting the main window must make it visible") &&
      check(gtk_widget_get_visible(ui.persistentStatusPane) != FALSE,
            "persistent status pane must be visible") &&
      check(gtk_widget_get_visible(ui.settingsPane) != FALSE,
            "settings pane must be visible");

  pipetune_gtk::destroyMainWindowUi(ui);
  g_object_unref(application);
  return valid && presentationWorks ? 0 : 1;
}
