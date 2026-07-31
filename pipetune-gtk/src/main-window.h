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
  /** Header-bar close action routed through transaction rollback. */
  GtkWidget *closeButton = nullptr;
  /** Persistent status pane beside every settings page. */
  GtkWidget *persistentStatusPane = nullptr;
  /** Settings pane containing the page switcher and stack. */
  GtkWidget *settingsPane = nullptr;
  /** Settings page switcher. */
  GtkWidget *settingsSwitcher = nullptr;
  /** Settings page stack. */
  GtkWidget *settingsStack = nullptr;
  /** Connection status image. */
  GtkWidget *statusImage = nullptr;
  /** Semantic status badge overlaid on the artwork. */
  GtkWidget *statusBadge = nullptr;
  /** Concise connection summary. */
  GtkWidget *connectionSummaryLabel = nullptr;
  /** Dynamic, always-expanded status section list. */
  GtkWidget *statusList = nullptr;
  /** Enables preset processing or selects bypass. */
  GtkWidget *processingEnabledSwitch = nullptr;
  /** EffeTune standard and saved preset drop-down. */
  GtkWidget *presetCombo = nullptr;
  /** EffeTune preset file chooser. */
  GtkWidget *presetChooser = nullptr;
  /** Opens the custom output device list. */
  GtkWidget *outputMenuButton = nullptr;
  /** Concise selected-output text. */
  GtkWidget *outputButtonLabel = nullptr;
  /** Output selection popover. */
  GtkWidget *outputPopover = nullptr;
  /** Dynamic output device rows. */
  GtkWidget *outputList = nullptr;
  /** Max or fixed DSP sample-rate drop-down. */
  GtkWidget *rateCombo = nullptr;
  /** PipeWire graph-rate suggestion or force drop-down. */
  GtkWidget *rateEnforcementCombo = nullptr;
  /** Scalar compatibility or SIMD acceleration backend drop-down. */
  GtkWidget *dspBackendCombo = nullptr;
  /** Conservative or exact-zero DSP idle policy drop-down. */
  GtkWidget *dspIdlePolicyCombo = nullptr;
  /** Restores the transaction to PipeTune defaults. */
  GtkWidget *restoreDefaultsButton = nullptr;
  /** PipeTune and EffeTune DSP version text. */
  GtkWidget *versionLabel = nullptr;
  /** Opens or closes the action-log drawer. */
  GtkWidget *logToggleButton = nullptr;
  /** Action-log toggle caption. */
  GtkWidget *logToggleLabel = nullptr;
  /** Animated action-log drawer. */
  GtkWidget *logRevealer = nullptr;
  /** All, warning, or error log filter. */
  GtkWidget *logFilterCombo = nullptr;
  /** Copies visible log entries. */
  GtkWidget *logCopyButton = nullptr;
  /** Clears retained log entries. */
  GtkWidget *logClearButton = nullptr;
  /** Dynamic action history rows. */
  GtkWidget *logList = nullptr;
  /** Concise dialog transaction state. */
  GtkWidget *transactionStateLabel = nullptr;
  /** Rolls live changes back and hides the dialog. */
  GtkWidget *cancelButton = nullptr;
  /** Persists the complete confirmed settings snapshot. */
  GtkWidget *applyButton = nullptr;
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
