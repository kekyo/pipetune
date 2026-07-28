#ifndef PIPETUNE_GTK_MAIN_WINDOW_H
#define PIPETUNE_GTK_MAIN_WINDOW_H

#include <gtk/gtk.h>

#include <string_view>

namespace pipetune_gtk {

/**
 * Holds the GtkBuilder and widgets required by the main window controller.
 */
struct MainWindowUi {
  /** Builder retaining the embedded UI object graph. */
  GtkBuilder *builder = nullptr;
  /** Top-level application window. */
  GtkWidget *window = nullptr;
  /** Connection status image. */
  GtkWidget *statusImage = nullptr;
  /** Connection status text. */
  GtkWidget *statusLabel = nullptr;
  /** Current DSP processing mode text. */
  GtkWidget *processingModeLabel = nullptr;
  /** Active preset path text. */
  GtkWidget *activePresetLabel = nullptr;
  /** Startup preset path text. */
  GtkWidget *startupPresetLabel = nullptr;
  /** Active DSP node count text. */
  GtkWidget *pluginCountLabel = nullptr;
  /** Selected output target text. */
  GtkWidget *targetLabel = nullptr;
  /** Default sink activity text. */
  GtkWidget *defaultSinkLabel = nullptr;
  /** Measured input frame-rate text. */
  GtkWidget *inputFrameRateLabel = nullptr;
  /** Latest input receipt text. */
  GtkWidget *lastInputLabel = nullptr;
  /** PCM data-rate text. */
  GtkWidget *pcmDataRateLabel = nullptr;
  /** PipeWire stream format text. */
  GtkWidget *streamFormatLabel = nullptr;
  /** Runtime error counter text. */
  GtkWidget *counterLabel = nullptr;
  /** Warning and diagnostic container. */
  GtkWidget *noticeBox = nullptr;
  /** Warning and diagnostic text. */
  GtkWidget *noticeLabel = nullptr;
  /** EffeTune preset file chooser. */
  GtkWidget *presetChooser = nullptr;
  /** Apply-and-save action button. */
  GtkWidget *applyButton = nullptr;
  /** Bypass-and-save action button. */
  GtkWidget *bypassButton = nullptr;
  /** Explicit status refresh button. */
  GtkWidget *refreshButton = nullptr;
  /** Notice dismissal button. */
  GtkWidget *dismissButton = nullptr;
};

/**
 * Builds the main window from the embedded GtkBuilder UI definition.
 *
 * Missing or incorrectly typed bundled objects are treated as fatal build
 * defects.
 *
 * @param application Application that owns the returned window.
 * @param title Runtime-generated window and header title.
 * @return Builder and borrowed widget pointers for the window controller.
 */
MainWindowUi createMainWindowUi(GtkApplication *application,
                                std::string_view title);

/**
 * Destroys the main window and releases its GtkBuilder.
 *
 * @param ui UI state to release and reset.
 */
void destroyMainWindowUi(MainWindowUi &ui) noexcept;

} // namespace pipetune_gtk

#endif
