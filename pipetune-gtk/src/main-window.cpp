#include "main-window.h"

#include "gtk-resources.h"

#include <string>

namespace pipetune_gtk {

constexpr char kMainWindowResourcePath[] =
    "/net/kekyo/pipetune-gtk/ui/main-window.ui";

static GtkWidget *requiredWidget(GtkBuilder *builder, const char *name,
                                 GType expectedType) {
  auto *object = gtk_builder_get_object(builder, name);
  if (object == nullptr || !g_type_is_a(G_OBJECT_TYPE(object),
                                        expectedType)) {
    g_error("PipeTune GTK UI object '%s' is missing or has the wrong type",
            name);
  }
  return GTK_WIDGET(object);
}

static void onAboutClicked(GtkButton *, gpointer userData) {
  auto *dialog = GTK_WIDGET(userData);
  gtk_widget_show_all(dialog);
  gtk_window_present_with_time(GTK_WINDOW(dialog),
                               gtk_get_current_event_time());
}

static void onAboutResponse(GtkDialog *dialog, gint, gpointer) {
  gtk_widget_hide(GTK_WIDGET(dialog));
}

MainWindowUi createMainWindowUi(GtkApplication *application,
                                std::string_view pipeTuneVersion,
                                std::string_view effetuneDspVersion) {
  ensureGtkResourcesRegistered();
  auto *builder = gtk_builder_new_from_resource(kMainWindowResourcePath);
  auto ui = MainWindowUi{
      .builder = builder,
      .window =
          requiredWidget(builder, "mainWindow",
                         GTK_TYPE_APPLICATION_WINDOW),
      .statusImage =
          requiredWidget(builder, "statusImage", GTK_TYPE_IMAGE),
      .statusBadge =
          requiredWidget(builder, "statusBadge", GTK_TYPE_IMAGE),
      .statusLabel =
          requiredWidget(builder, "statusLabel", GTK_TYPE_LABEL),
      .processingModeLabel =
          requiredWidget(builder, "processingModeLabel",
                         GTK_TYPE_LABEL),
      .activePresetLabel =
          requiredWidget(builder, "activePresetLabel", GTK_TYPE_LABEL),
      .startupPresetLabel =
          requiredWidget(builder, "startupPresetLabel", GTK_TYPE_LABEL),
      .pluginCountLabel =
          requiredWidget(builder, "pluginCountLabel", GTK_TYPE_LABEL),
      .outputCombo =
          requiredWidget(builder, "outputCombo",
                         GTK_TYPE_COMBO_BOX_TEXT),
      .targetLabel =
          requiredWidget(builder, "targetLabel", GTK_TYPE_LABEL),
      .outputReasonLabel =
          requiredWidget(builder, "outputReasonLabel", GTK_TYPE_LABEL),
      .defaultSinkLabel =
          requiredWidget(builder, "defaultSinkLabel", GTK_TYPE_LABEL),
      .inputFrameRateLabel =
          requiredWidget(builder, "inputFrameRateLabel", GTK_TYPE_LABEL),
      .lastInputLabel =
          requiredWidget(builder, "lastInputLabel", GTK_TYPE_LABEL),
      .pcmDataRateLabel =
          requiredWidget(builder, "pcmDataRateLabel", GTK_TYPE_LABEL),
      .streamFormatLabel =
          requiredWidget(builder, "streamFormatLabel", GTK_TYPE_LABEL),
      .dspProcessingTimeLabel =
          requiredWidget(builder, "dspProcessingTimeLabel",
                         GTK_TYPE_LABEL),
      .counterLabel =
          requiredWidget(builder, "counterLabel", GTK_TYPE_LABEL),
      .noticeBox = requiredWidget(builder, "noticeBox", GTK_TYPE_BOX),
      .noticeLabel =
          requiredWidget(builder, "noticeLabel", GTK_TYPE_LABEL),
      .presetChooser =
          requiredWidget(builder, "presetChooser",
                         GTK_TYPE_FILE_CHOOSER_BUTTON),
      .applyButton =
          requiredWidget(builder, "applyButton", GTK_TYPE_BUTTON),
      .bypassButton =
          requiredWidget(builder, "bypassButton", GTK_TYPE_BUTTON),
      .dismissButton =
          requiredWidget(builder, "dismissButton", GTK_TYPE_BUTTON),
      .aboutButton =
          requiredWidget(builder, "aboutButton", GTK_TYPE_BUTTON),
      .aboutDialog = gtk_about_dialog_new(),
  };
  auto *headerBar =
      requiredWidget(builder, "headerBar", GTK_TYPE_HEADER_BAR);
  const auto pipeTuneVersionText = std::string(pipeTuneVersion);
  const auto effetuneDspVersionText =
      "EffeTune DSP " + std::string(effetuneDspVersion);
  gtk_window_set_application(GTK_WINDOW(ui.window), application);
  gtk_window_set_title(GTK_WINDOW(ui.window), "PipeTune");
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerBar), "PipeTune");

  gtk_window_set_title(GTK_WINDOW(ui.aboutDialog), "About PipeTune");
  gtk_window_set_transient_for(GTK_WINDOW(ui.aboutDialog),
                               GTK_WINDOW(ui.window));
  gtk_window_set_destroy_with_parent(GTK_WINDOW(ui.aboutDialog), TRUE);
  gtk_window_set_modal(GTK_WINDOW(ui.aboutDialog), TRUE);
  gtk_about_dialog_set_program_name(GTK_ABOUT_DIALOG(ui.aboutDialog),
                                    "PipeTune");
  gtk_about_dialog_set_version(GTK_ABOUT_DIALOG(ui.aboutDialog),
                               pipeTuneVersionText.c_str());
  gtk_about_dialog_set_comments(GTK_ABOUT_DIALOG(ui.aboutDialog),
                                effetuneDspVersionText.c_str());
  g_signal_connect(ui.aboutButton, "clicked", G_CALLBACK(onAboutClicked),
                   ui.aboutDialog);
  g_signal_connect(ui.aboutDialog, "response",
                   G_CALLBACK(onAboutResponse), nullptr);
  g_signal_connect(ui.aboutDialog, "delete-event",
                   G_CALLBACK(gtk_widget_hide_on_delete), nullptr);

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
  gtk_window_present_with_time(GTK_WINDOW(ui.window),
                               userInteractionTime);
}

void destroyMainWindowUi(MainWindowUi &ui) noexcept {
  if (ui.aboutDialog != nullptr) {
    gtk_widget_destroy(ui.aboutDialog);
  }
  if (ui.window != nullptr) {
    gtk_widget_destroy(ui.window);
  }
  if (ui.builder != nullptr) {
    g_object_unref(ui.builder);
  }
  ui = {};
}

} // namespace pipetune_gtk
