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
         check(GTK_IS_LABEL(ui.targetLabel),
               "target label type differs") &&
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
         check(GTK_IS_LABEL(ui.counterLabel),
               "counter label type differs") &&
         check(GTK_IS_BOX(ui.noticeBox),
               "notice box type differs") &&
         check(GTK_IS_LABEL(ui.noticeLabel),
               "notice label type differs") &&
         check(GTK_IS_FILE_CHOOSER_BUTTON(ui.presetChooser),
               "preset chooser type differs") &&
         check(GTK_IS_BUTTON(ui.applyButton),
               "apply button type differs") &&
         check(GTK_IS_BUTTON(ui.bypassButton),
               "bypass button type differs") &&
         check(GTK_IS_BUTTON(ui.refreshButton),
               "refresh button type differs") &&
         check(GTK_IS_BUTTON(ui.dismissButton),
               "dismiss button type differs");
}

int main(int argc, char **argv) {
  if (!gtk_init_check(&argc, &argv)) {
    std::cerr << "GTK display is unavailable\n";
    return 1;
  }
  auto *application = gtk_application_new(
      "net.kekyo.pipetune-gtk.tests", G_APPLICATION_NON_UNIQUE);
  auto ui = pipetune_gtk::createMainWindowUi(
      application, "PipeTune GTK test");

  auto width = int{0};
  auto height = int{0};
  gtk_window_get_default_size(GTK_WINDOW(ui.window), &width, &height);
  const auto valid =
      check(ui.builder != nullptr, "main window builder is unavailable") &&
      checkWidgetTypes(ui) &&
      check(gtk_window_get_application(GTK_WINDOW(ui.window)) ==
                application,
            "main window application differs") &&
      check(std::string_view(
                gtk_window_get_title(GTK_WINDOW(ui.window))) ==
                "PipeTune GTK test",
            "main window title differs") &&
      check(width == 680 && height == 580,
            "main window default size differs") &&
      check(gtk_widget_get_hexpand(ui.statusLabel) != FALSE,
            "status label must expand");

  pipetune_gtk::destroyMainWindowUi(ui);
  g_object_unref(application);
  return valid ? 0 : 1;
}
