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

MainWindowUi createMainWindowUi(GtkApplication *application,
                                std::string_view title) {
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
      .targetLabel =
          requiredWidget(builder, "targetLabel", GTK_TYPE_LABEL),
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
      .refreshButton =
          requiredWidget(builder, "refreshButton", GTK_TYPE_BUTTON),
      .dismissButton =
          requiredWidget(builder, "dismissButton", GTK_TYPE_BUTTON),
  };
  auto *headerBar =
      requiredWidget(builder, "headerBar", GTK_TYPE_HEADER_BAR);
  const auto titleText = std::string(title);
  gtk_window_set_application(GTK_WINDOW(ui.window), application);
  gtk_window_set_title(GTK_WINDOW(ui.window), titleText.c_str());
  gtk_header_bar_set_title(GTK_HEADER_BAR(headerBar), titleText.c_str());

  auto *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "EffeTune presets");
  gtk_file_filter_add_pattern(filter, "*.effetune_preset");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(ui.presetChooser), filter);
  return ui;
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
