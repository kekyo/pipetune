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
         check(GTK_IS_IMAGE(ui.statusImage),
               "status image type differs") &&
         check(GTK_IS_IMAGE(ui.statusBadge),
               "status badge type differs") &&
         check(GTK_IS_LABEL(ui.statusLabel),
               "status label type differs") &&
         check(GTK_IS_LABEL(ui.processingModeLabel),
               "processing mode label type differs") &&
         check(GTK_IS_LABEL(ui.activePresetLabel),
               "active preset label type differs") &&
         check(GTK_IS_LABEL(ui.startupPresetLabel),
               "startup preset label type differs") &&
         check(GTK_IS_LABEL(ui.pluginCountLabel),
               "plugin count label type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.outputCombo),
               "output combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.rateCombo),
               "rate combo-box type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.rateEnforcementCombo),
               "rate enforcement combo-box type differs") &&
         check(GTK_IS_BUTTON(ui.rateApplyButton),
               "rate apply button type differs") &&
         check(GTK_IS_LABEL(ui.rateStatusLabel),
               "rate status label type differs") &&
         check(GTK_IS_LABEL(ui.targetLabel),
               "target label type differs") &&
         check(GTK_IS_LABEL(ui.outputReasonLabel),
               "output reason label type differs") &&
         check(GTK_IS_LABEL(ui.defaultSinkLabel),
               "default sink label type differs") &&
         check(GTK_IS_LABEL(ui.inputFrameRateLabel),
               "input frame-rate label type differs") &&
         check(GTK_IS_LABEL(ui.lastInputLabel),
               "last input label type differs") &&
         check(GTK_IS_LABEL(ui.pcmDataRateLabel),
               "PCM data-rate label type differs") &&
         check(GTK_IS_LABEL(ui.streamFormatLabel),
               "stream format label type differs") &&
         check(GTK_IS_LABEL(ui.dspProcessingTimeLabel),
               "DSP processing-time label type differs") &&
         check(GTK_IS_LABEL(ui.counterLabel),
               "counter label type differs") &&
         check(GTK_IS_LABEL(ui.versionLabel),
               "version label type differs") &&
         check(GTK_IS_BOX(ui.noticeBox),
               "notice box type differs") &&
         check(GTK_IS_LABEL(ui.noticeLabel),
               "notice label type differs") &&
         check(GTK_IS_FILE_CHOOSER_BUTTON(ui.presetChooser),
               "preset chooser type differs") &&
         check(GTK_IS_COMBO_BOX_TEXT(ui.presetCombo),
               "EffeTune preset combo-box type differs") &&
         check(GTK_IS_BUTTON(ui.applyButton),
               "apply button type differs") &&
         check(GTK_IS_BUTTON(ui.bypassButton),
               "bypass button type differs") &&
         check(GTK_IS_BUTTON(ui.resetButton),
               "configuration reset button type differs") &&
         check(GTK_IS_BUTTON(ui.dismissButton),
               "dismiss button type differs");
}

static bool checkInlineVersion(
    const pipetune_gtk::MainWindowUi &ui) {
  auto *outputGrid =
      gtk_builder_get_object(ui.builder, "outputStatusGrid");
  if (!check(GTK_IS_GRID(outputGrid),
             "output status grid type differs")) {
    return false;
  }
  auto *versionTitle =
      gtk_grid_get_child_at(GTK_GRID(outputGrid), 0, 2);
  auto *versionValue =
      gtk_grid_get_child_at(GTK_GRID(outputGrid), 1, 2);
  return check(GTK_IS_LABEL(versionTitle),
               "version title type differs") &&
         check(std::string_view(
                   gtk_label_get_text(GTK_LABEL(versionTitle))) ==
                   "Versions",
               "version title differs") &&
         check(versionValue == ui.versionLabel,
               "versions must follow Selection reason") &&
         check(std::string_view(
                   gtk_label_get_text(GTK_LABEL(ui.versionLabel))) ==
                   "PipeTune 1.2.3  •  EffeTune DSP 4.5.6",
               "inline version text differs");
}

static bool checkInputStreamFormatTitle(
    const pipetune_gtk::MainWindowUi &ui) {
  auto *statusGrid =
      gtk_builder_get_object(ui.builder, "runtimeStatusGrid");
  if (!check(GTK_IS_GRID(statusGrid),
             "runtime status grid type differs")) {
    return false;
  }
  auto *title = gtk_grid_get_child_at(GTK_GRID(statusGrid), 0, 9);
  return check(GTK_IS_LABEL(title),
               "input stream format title type differs") &&
         check(std::string_view(
                   gtk_label_get_text(GTK_LABEL(title))) ==
                   "Input stream format",
               "input stream format title differs");
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
  auto *headerBar = gtk_builder_get_object(ui.builder, "headerBar");
  const auto valid =
      check(ui.builder != nullptr, "main window builder is unavailable") &&
      checkWidgetTypes(ui) &&
      checkInlineVersion(ui) &&
      checkInputStreamFormatTitle(ui) &&
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
      check(width == 680 && height == 720,
            "main window default size differs") &&
      check(gtk_builder_get_object(ui.builder, "refreshButton") == nullptr,
            "periodically updated status must not expose a refresh button") &&
      check(std::string_view(
                gtk_button_get_label(GTK_BUTTON(ui.resetButton))) ==
                "Reset Configuration…",
            "configuration reset button label differs") &&
      check(gtk_widget_get_hexpand(ui.statusLabel) != FALSE,
            "status label must expand");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.outputCombo),
                                 "System default");
  gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(ui.outputCombo),
                                 "USB Headphones");
  gtk_combo_box_set_active(GTK_COMBO_BOX(ui.outputCombo), 1);
  const auto selectionWorks =
      check(gtk_combo_box_get_active(GTK_COMBO_BOX(ui.outputCombo)) == 1,
            "output combo-box selection differs");

  gtk_widget_hide(ui.window);
  pipetune_gtk::presentMainWindow(ui, 1234);
  const auto presentationWorks =
      check(gtk_widget_get_visible(ui.window) != FALSE,
            "presenting the main window must make it visible");

  pipetune_gtk::destroyMainWindowUi(ui);
  g_object_unref(application);
  return valid && selectionWorks && presentationWorks ? 0 : 1;
}
