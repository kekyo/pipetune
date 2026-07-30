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
  /** Semantic status badge overlaid on the artwork. */
  GtkWidget *statusBadge = nullptr;
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
  /** User-preferred physical output drop-down. */
  GtkWidget *outputCombo = nullptr;
  /** Max or fixed DSP sample-rate drop-down. */
  GtkWidget *rateCombo = nullptr;
  /** PipeWire graph-rate suggestion or force drop-down. */
  GtkWidget *rateEnforcementCombo = nullptr;
  /** Applies and persists the edited PCM rate policy. */
  GtkWidget *rateApplyButton = nullptr;
  /** Final input/DSP, output, and active physical rates. */
  GtkWidget *rateStatusLabel = nullptr;
  /** Scalar compatibility or SIMD acceleration backend drop-down. */
  GtkWidget *dspBackendCombo = nullptr;
  /** Applies and persists the edited native DSP backend. */
  GtkWidget *dspBackendApplyButton = nullptr;
  /** Configured, effective, and fallback backend state. */
  GtkWidget *dspBackendStatusLabel = nullptr;
  /** Conservative or exact-zero DSP idle policy drop-down. */
  GtkWidget *dspIdlePolicyCombo = nullptr;
  /** Applies and persists the edited DSP idle policy. */
  GtkWidget *dspIdlePolicyApplyButton = nullptr;
  /** DSP idle state, counters, and PipeWire graph idle state. */
  GtkWidget *dspIdleStatusLabel = nullptr;
  /** Selected output target text. */
  GtkWidget *targetLabel = nullptr;
  /** Engine-owned output-selection reason text. */
  GtkWidget *outputReasonLabel = nullptr;
  /** PipeTune and EffeTune DSP version text. */
  GtkWidget *versionLabel = nullptr;
  /** Default sink activity text. */
  GtkWidget *defaultSinkLabel = nullptr;
  /** Measured input frame-rate text. */
  GtkWidget *inputFrameRateLabel = nullptr;
  /** Latest input receipt text. */
  GtkWidget *lastInputLabel = nullptr;
  /** PCM data-rate text. */
  GtkWidget *pcmDataRateLabel = nullptr;
  /** Readable PipeWire input stream format text. */
  GtkWidget *streamFormatLabel = nullptr;
  /** Average native EffeTune processing time text. */
  GtkWidget *dspProcessingTimeLabel = nullptr;
  /** Runtime error counter text. */
  GtkWidget *counterLabel = nullptr;
  /** Warning and diagnostic container. */
  GtkWidget *noticeBox = nullptr;
  /** Warning and diagnostic text. */
  GtkWidget *noticeLabel = nullptr;
  /** EffeTune preset file chooser. */
  GtkWidget *presetChooser = nullptr;
  /** EffeTune standard and saved preset drop-down. */
  GtkWidget *presetCombo = nullptr;
  /** Apply-and-save action button. */
  GtkWidget *applyButton = nullptr;
  /** Bypass-and-save action button. */
  GtkWidget *bypassButton = nullptr;
  /** Restores all PipeTune startup choices to their defaults. */
  GtkWidget *resetButton = nullptr;
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
 * @param pipeTuneVersion PipeTune package version shown in the status area.
 * @param effetuneDspVersion EffeTune DSP version shown in the status area.
 * @return Builder and borrowed widget pointers for the window controller.
 */
MainWindowUi createMainWindowUi(GtkApplication *application,
                                std::string_view pipeTuneVersion,
                                std::string_view effetuneDspVersion);

/**
 * Shows and presents the main window for a user interaction.
 *
 * @param ui UI state containing the main window.
 * @param userInteractionTime GDK timestamp captured from the interaction, or
 * GDK_CURRENT_TIME when the source does not provide one.
 */
void presentMainWindow(const MainWindowUi &ui,
                       guint32 userInteractionTime) noexcept;

/**
 * Destroys the main window and releases its GtkBuilder.
 *
 * @param ui UI state to release and reset.
 */
void destroyMainWindowUi(MainWindowUi &ui) noexcept;

} // namespace pipetune_gtk

#endif
