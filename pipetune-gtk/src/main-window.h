#ifndef PIPETUNE_GTK_MAIN_WINDOW_H
#define PIPETUNE_GTK_MAIN_WINDOW_H

#include <gtk/gtk.h>

#include <optional>
#include <string>
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
  /** Resizable divider between status and settings panes. */
  GtkWidget *mainPaned = nullptr;
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
  /** DSP Load meter host aligned below the connection labels. */
  GtkWidget *statusLoadMeterBox = nullptr;
  /** Dynamic, always-expanded status section list. */
  GtkWidget *statusList = nullptr;
  /** Enables preset processing or selects bypass. */
  GtkWidget *processingEnabledSwitch = nullptr;
  /** EffeTune standard and saved preset drop-down. */
  GtkWidget *presetCombo = nullptr;
  /** EffeTune preset file chooser. */
  GtkWidget *presetChooser = nullptr;
  /** Automatic or fixed DSP sample-rate drop-down. */
  GtkWidget *rateCombo = nullptr;
  /** PipeWire graph-rate suggestion or force drop-down. */
  GtkWidget *rateEnforcementCombo = nullptr;
  /** Scalar compatibility or SIMD acceleration backend drop-down. */
  GtkWidget *dspBackendCombo = nullptr;
  /** Supported presentation-language drop-down. */
  GtkWidget *languageCombo = nullptr;
  /** Inline notice that a restart is required to apply a language change. */
  GtkWidget *languageRestartNotice = nullptr;
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
 * Captures presentation-only state retained across a GtkBuilder rebuild.
 */
struct MainWindowViewState {
  /** Stable GtkStack child name for the selected settings page. */
  std::string settingsPage;
  /** Horizontal status/settings pane divider position. */
  int mainPanedPosition;
  /** Whether the action-log drawer is expanded. */
  bool logVisible;
  /** Zero-based action-log filter selection. */
  int logFilter;
  /** Current window width in logical pixels. */
  int windowWidth;
  /** Current window height in logical pixels. */
  int windowHeight;
  /** Current window horizontal position. */
  int windowX;
  /** Current window vertical position. */
  int windowY;
  /** Whether the window is maximized. */
  bool maximized;
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
 * Resolves the interaction timestamp used to present the main window.
 *
 * A supplied event timestamp is preserved. When no timestamp is available,
 * a fresh X server timestamp is requested from a realized X11 window so the
 * window manager can associate the presentation with the tray activation.
 *
 * @param window Main application window.
 * @param userInteractionTime Timestamp supplied by the activation source.
 * @return Supplied or freshly resolved timestamp, or no value when the
 * platform cannot provide one.
 */
std::optional<guint32> mainWindowPresentationTime(
    GtkWidget *window,
    std::optional<guint32> userInteractionTime) noexcept;

/**
 * Shows and presents the main window for a user interaction.
 *
 * @param ui UI state containing the main window.
 * @param userInteractionTime GDK timestamp captured from the interaction, or
 * no value when the source does not provide one.
 */
void presentMainWindow(const MainWindowUi &ui,
                       std::optional<guint32> userInteractionTime) noexcept;

/**
 * Shows or fully removes the action-log drawer from window allocation.
 *
 * @param ui Loaded main-window widgets.
 * @param visible True to reveal the drawer, false to collapse it to zero
 * height.
 */
void setLogDrawerVisible(const MainWindowUi &ui, bool visible) noexcept;

/**
 * Captures state owned by the current main-window presentation.
 *
 * @param ui Loaded main-window widgets.
 * @return View state suitable for a replacement GtkBuilder presentation.
 */
MainWindowViewState
captureMainWindowViewState(const MainWindowUi &ui) noexcept;

/**
 * Restores presentation-only state after a GtkBuilder rebuild.
 *
 * @param ui Replacement main-window widgets.
 * @param state Previously captured view state.
 */
void restoreMainWindowViewState(
    const MainWindowUi &ui,
    const MainWindowViewState &state) noexcept;

/**
 * Destroys the main window and releases its GtkBuilder.
 *
 * @param ui UI state to release and reset.
 */
void destroyMainWindowUi(MainWindowUi &ui) noexcept;

} // namespace pipetune_gtk

#endif
