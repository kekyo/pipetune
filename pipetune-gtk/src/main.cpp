#include "action-log.h"
#include "application-state.h"
#include "control-client.h"
#include "dsp-backend-selection-model.h"
#include "installed-locales.h"
#include "installed-presets.h"
#include "launch-options.h"
#include "localization.h"
#include "main-window.h"
#include "preset-catalog.h"
#include "preset-file-monitor.h"
#include "rate-selection-model.h"
#include "settings-transaction.h"
#include "status-icon.h"
#include "status-level-meter.h"
#include "status-model.h"
#include "tray-backend.h"
#include "ui-language.h"
#include "ui-message.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"
#include "pipetune/version.h"

#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
#include <gestament/gtk.h>
#endif

#include <gtk/gtk.h>

#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pipetune_gtk {

constexpr auto kReconnectDelaySeconds = guint{2};
constexpr auto kStatusArtworkSize = int{48};
constexpr auto kActionLogCapacity = std::size_t{500};
constexpr auto kDspLoadStatusId = std::string_view{"dsp.load"};

struct StatusRowWidgets {
  GtkWidget *text;
};

struct GtkRuntime {
  GtkApplication *application;
  ApplicationState state;
  ControlClient *controlClient;
  TrayBackendState *trayBackend;
  TrayBackendAvailabilityState trayAvailability;
  std::filesystem::path startupConfigPath;
  pipetune::StartupConfig savedConfig;
  bool startupConfigAvailable;
  UiLocalizationEnvironment originalLocalization;
  std::filesystem::path uiLanguageConfigPath;
  UiLanguage presentationLanguage;
  UiLanguage uiLanguage;
  bool languageRestartRequired;
  std::string uiLanguageLoadWarning;
  std::string localizationWarning;
  SettingsTransaction transaction;
  bool transactionReady;
  bool dialogActive;
  bool updatingControls;
  bool closeAfterRollback;
  bool quitAfterRollback;
  std::filesystem::path lastPresetPath;
  std::vector<PresetChoice> presetChoices;
  std::filesystem::path effetuneUserPresetPath;
  EffeTunePresetFileMonitor *presetFileMonitor;
  std::string presetCatalogSourceDiagnostic;
  std::string presetCatalogSavedDiagnostic;
  std::vector<SampleRateChoice> rateChoices;
  std::vector<DspBackendChoice> dspBackendChoices;
  std::map<std::string, StatusRowWidgets> statusRows;
  StatusLevelMeterWidgets statusLoadMeter;
  ActionLog actionLog;
  ActionLogFilter logFilter;
  std::uint64_t pendingActionId;
  guint reconnectSource;
  bool applicationHeld;
  bool activationHandled;
  bool shuttingDown;
  bool quitting;
  MainWindowUi ui;
  GdkPixbuf *statusColorIcon;
  GdkPixbuf *statusGrayscaleIcon;
};

static void render(GtkRuntime *runtime);
static void driveSettings(GtkRuntime *runtime);
static void beginTransactionFromRuntime(GtkRuntime *runtime);
static void presentWindow(GtkRuntime *runtime,
                          guint32 userInteractionTime);
static void requestQuit(GtkRuntime *runtime);
static void scheduleReconnect(GtkRuntime *runtime);

static std::string versionText() {
  return "PipeTune GTK " + std::string(pipetune::version()) +
         ", EffeTune DSP " + std::string(pipetune::effetuneVersion());
}

static std::int64_t currentMonotonicMilliseconds() noexcept {
  return static_cast<std::int64_t>(g_get_monotonic_time() / 1000);
}

static std::uint64_t currentUnixMilliseconds() noexcept {
  return static_cast<std::uint64_t>(g_get_real_time() / 1000);
}

static pipetune::StartupConfig defaultStartupConfig() {
  return {
      .presetFound = false,
      .presetPath = {},
      .ratePolicy = pipetune::defaultSampleRatePolicy(),
      .dspBackend = pipetune::DspBackendKind::scalar,
      .dspSimdVariant = pipetune::DspSimdVariant::automatic,
  };
}

static void appendDetail(std::string &text, std::string_view detail) {
  if (detail.empty()) {
    return;
  }
  if (!text.empty()) {
    text.push_back('\n');
  }
  text.append(detail);
}

static std::string controlDiagnostic(const ControlClientReply &reply) {
  if (!reply.transportError.empty()) {
    return reply.transportError;
  }
  if (!reply.response.error.empty()) {
    return reply.response.error;
  }
  if (!reply.response.valid) {
    return "PipeTune returned an invalid control reply";
  }
  return "PipeTune rejected the requested setting";
}

static TrayIconState iconStateForApplication(
    const ApplicationState &state) {
  const auto visual = trayVisualState(state);
  if (visual == TrayVisualState::attention) {
    return TrayIconState::attention;
  }
  if (visual == TrayVisualState::disconnected) {
    return TrayIconState::disconnected;
  }
  return TrayIconState::active;
}

static std::string connectionSummary(const ApplicationState &state) {
  switch (state.connection) {
  case ControlConnectionState::connecting:
    return translate("Connecting to the control service…");
  case ControlConnectionState::disconnected:
    return translate("Control service unavailable");
  case ControlConnectionState::connected:
    break;
  }
  if (state.hasRuntimeStatus && state.runtime.rateTransitioning) {
    return translate("Connected · changing sample rate");
  }
  return translate("Connected and monitoring");
}

static std::string trayTooltip(const ApplicationState &state) {
  if (state.connection == ControlConnectionState::connecting) {
    return translate("PipeTune: connecting");
  }
  if (state.connection == ControlConnectionState::disconnected) {
    return translate("PipeTune: disconnected");
  }
  if (trayVisualState(state) == TrayVisualState::attention) {
    return translate("PipeTune: attention required");
  }
  const auto filename =
      std::filesystem::path(state.runtime.activePreset).filename().string();
  return filename.empty()
             ? std::string(translate("PipeTune: active"))
             : formatUiMessage(
                   localizedMessage("PipeTune: {0}", {filename}));
}

static const char *badgeIconName(StatusBadge badge) {
  if (badge == StatusBadge::attention) {
    return "dialog-warning-symbolic";
  }
  if (badge == StatusBadge::disconnected) {
    return "network-offline-symbolic";
  }
  return nullptr;
}

static void initializeStatusArtwork(GtkRuntime *runtime) {
  runtime->statusColorIcon = loadPipeTuneIconPixbuf(
      kStatusArtworkSize, TrayIconColorMode::color);
  runtime->statusGrayscaleIcon = loadPipeTuneIconPixbuf(
      kStatusArtworkSize, TrayIconColorMode::grayscale);
  if (runtime->statusColorIcon == nullptr ||
      runtime->statusGrayscaleIcon == nullptr) {
    g_error("PipeTune GTK status artwork could not be loaded");
  }
}

static void releaseStatusArtwork(GtkRuntime *runtime) noexcept {
  if (runtime->statusColorIcon != nullptr) {
    g_object_unref(runtime->statusColorIcon);
    runtime->statusColorIcon = nullptr;
  }
  if (runtime->statusGrayscaleIcon != nullptr) {
    g_object_unref(runtime->statusGrayscaleIcon);
    runtime->statusGrayscaleIcon = nullptr;
  }
}

static void addStyleClass(GtkWidget *widget, const char *name) {
  gtk_style_context_add_class(gtk_widget_get_style_context(widget), name);
}

static std::string accessibleStatusId(std::string_view modelId) {
  auto id = std::string("status-");
  id.reserve(id.size() + modelId.size());
  for (const auto character : modelId) {
    id.push_back(character == '.' ? '-' : character);
  }
  return id;
}

static void assignDynamicAccessibleId(GtkWidget *widget,
                                      const std::string &id) {
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
  gestament_gtk_assign_accessible_id(widget, id.c_str());
#else
  static_cast<void>(widget);
  static_cast<void>(id);
#endif
}

static GtkWidget *createStatusSectionRow(const StatusSection &section) {
  auto *row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  auto *label = gtk_label_new(section.label.c_str());
  gtk_label_set_xalign(GTK_LABEL(label), 0.0F);
  addStyleClass(label, "status-section");
  gtk_container_add(GTK_CONTAINER(row), label);
  return row;
}

static StatusRowWidgets createStatusItemRow(const StatusItem &item,
                                            GtkWidget **rowOut) {
  auto *row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 14);
  addStyleClass(box, "status-row");
  auto *title = gtk_label_new(item.label.c_str());
  gtk_label_set_xalign(GTK_LABEL(title), 0.0F);
  gtk_widget_set_hexpand(title, FALSE);
  addStyleClass(title, "dim-label");
  gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);

  auto *text = gtk_label_new(item.value.c_str());
  gtk_label_set_xalign(GTK_LABEL(text), 1.0F);
  gtk_label_set_ellipsize(GTK_LABEL(text), PANGO_ELLIPSIZE_END);
  gtk_label_set_max_width_chars(GTK_LABEL(text), 28);
  gtk_label_set_selectable(GTK_LABEL(text), TRUE);
  gtk_widget_set_hexpand(text, TRUE);
  gtk_box_pack_end(GTK_BOX(box), text, TRUE, TRUE, 0);
  gtk_container_add(GTK_CONTAINER(row), box);
  const auto statusId = accessibleStatusId(item.id);
  assignDynamicAccessibleId(text, statusId);
  *rowOut = row;
  return {.text = text};
}

static void initializeStatusRows(GtkRuntime *runtime) {
  runtime->statusRows.clear();
  runtime->statusLoadMeter = createStatusLevelMeter();
  gtk_box_pack_start(GTK_BOX(runtime->ui.statusLoadMeterBox),
                     runtime->statusLoadMeter.root, TRUE, TRUE, 0);
  assignDynamicAccessibleId(runtime->statusLoadMeter.levelBar,
                            "status-dsp-load-meter");
  gtk_widget_show_all(runtime->ui.statusLoadMeterBox);
  const auto sections = buildStatusSections(
      runtime->state, runtime->savedConfig, currentUnixMilliseconds());
  for (const auto &section : sections) {
    auto *heading = createStatusSectionRow(section);
    gtk_list_box_insert(GTK_LIST_BOX(runtime->ui.statusList), heading, -1);
    for (const auto &item : section.items) {
      if (item.id == kDspLoadStatusId) {
        continue;
      }
      auto *row = static_cast<GtkWidget *>(nullptr);
      auto widgets = createStatusItemRow(item, &row);
      gtk_list_box_insert(GTK_LIST_BOX(runtime->ui.statusList), row, -1);
      runtime->statusRows.emplace(item.id, widgets);
    }
  }
  gtk_widget_show_all(runtime->ui.statusList);
}

static void removeStatusSeverityClasses(GtkWidget *widget) {
  auto *context = gtk_widget_get_style_context(widget);
  gtk_style_context_remove_class(context, "status-warning");
  gtk_style_context_remove_class(context, "status-error");
}

static void renderStatusLoadMeter(GtkRuntime *runtime,
                                  const StatusItem &item) {
  const auto level = statusLevelPresentation(item);
  const auto accessibleName = item.label + " " + item.value;
  updateStatusLevelMeter(
      runtime->statusLoadMeter,
      {
          .minimum = level.has_value() ? *item.minimum : 0.0,
          .maximum = level.has_value() ? *item.maximum : 100.0,
          .value = level.has_value() ? level->clampedValue : 0.0,
          .hueStep = level.has_value() ? level->hueStep
                                       : std::uint8_t{0},
          .valueText = item.value,
          .accessibleName = accessibleName,
          .accessibleDescription = item.tooltip,
      });
  gtk_widget_set_tooltip_text(
      runtime->statusLoadMeter.root,
      item.tooltip.empty() ? nullptr : item.tooltip.c_str());
}

static void renderStatusRows(GtkRuntime *runtime) {
  const auto &saved = runtime->transactionReady
                          ? runtime->transaction.saved
                          : runtime->savedConfig;
  const auto sections = buildStatusSections(
      runtime->state, saved, currentUnixMilliseconds());
  for (const auto &section : sections) {
    for (const auto &item : section.items) {
      if (item.id == kDspLoadStatusId) {
        renderStatusLoadMeter(runtime, item);
        continue;
      }
      const auto found = runtime->statusRows.find(item.id);
      if (found == runtime->statusRows.end()) {
        continue;
      }
      auto &widgets = found->second;
      gtk_label_set_text(GTK_LABEL(widgets.text), item.value.c_str());
      gtk_widget_set_tooltip_text(
          widgets.text, item.tooltip.empty() ? nullptr : item.tooltip.c_str());
      removeStatusSeverityClasses(widgets.text);
      if (item.severity == StatusSeverity::warning) {
        addStyleClass(widgets.text, "status-warning");
      } else if (item.severity == StatusSeverity::error) {
        addStyleClass(widgets.text, "status-error");
      }
    }
  }
}

static void clearContainer(GtkWidget *container) {
  auto *children = gtk_container_get_children(GTK_CONTAINER(container));
  for (auto *child = children; child != nullptr; child = child->next) {
    gtk_widget_destroy(GTK_WIDGET(child->data));
  }
  g_list_free(children);
}

static std::string formatActionTime(std::uint64_t unixMilliseconds) {
  const auto raw =
      static_cast<std::time_t>(unixMilliseconds / std::uint64_t{1000});
  auto local = std::tm{};
  localtime_r(&raw, &local);
  auto stream = std::ostringstream{};
  stream << std::put_time(&local, "%H:%M:%S");
  return stream.str();
}

static const char *actionStateSymbol(const ActionLogEntry &entry) {
  if (entry.state == ActionLogState::pending) {
    return "…";
  }
  if (entry.state == ActionLogState::failure) {
    return "!";
  }
  return "✓";
}

static GtkWidget *createActionLogRow(const ActionLogEntry &entry) {
  auto *row = gtk_list_box_row_new();
  gtk_list_box_row_set_selectable(GTK_LIST_BOX_ROW(row), FALSE);
  gtk_list_box_row_set_activatable(GTK_LIST_BOX_ROW(row), FALSE);
  auto *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 10);
  gtk_widget_set_margin_start(box, 14);
  gtk_widget_set_margin_end(box, 14);
  gtk_widget_set_margin_top(box, 8);
  gtk_widget_set_margin_bottom(box, 8);
  auto *symbol = gtk_label_new(actionStateSymbol(entry));
  gtk_widget_set_valign(symbol, GTK_ALIGN_START);
  if (entry.severity == ActionLogSeverity::warning) {
    addStyleClass(symbol, "status-warning");
  } else if (entry.severity == ActionLogSeverity::error) {
    addStyleClass(symbol, "status-error");
  }
  gtk_box_pack_start(GTK_BOX(box), symbol, FALSE, FALSE, 0);
  auto *text = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
  gtk_widget_set_hexpand(text, TRUE);
  const auto summaryText = formatUiMessage(entry.summary);
  auto *summary = gtk_label_new(summaryText.c_str());
  gtk_label_set_xalign(GTK_LABEL(summary), 0.0F);
  gtk_label_set_ellipsize(GTK_LABEL(summary), PANGO_ELLIPSIZE_END);
  gtk_box_pack_start(GTK_BOX(text), summary, FALSE, TRUE, 0);
  if (!uiMessageIsEmpty(entry.detail)) {
    const auto detailText = formatUiMessage(entry.detail);
    auto *detail = gtk_label_new(detailText.c_str());
    gtk_label_set_xalign(GTK_LABEL(detail), 0.0F);
    gtk_label_set_line_wrap(GTK_LABEL(detail), TRUE);
    addStyleClass(detail, "log-detail");
    gtk_box_pack_start(GTK_BOX(text), detail, FALSE, TRUE, 0);
  }
  gtk_box_pack_start(GTK_BOX(box), text, TRUE, TRUE, 0);
  const auto timestamp = formatActionTime(entry.timestampUnixMilliseconds);
  auto *time = gtk_label_new(timestamp.c_str());
  gtk_widget_set_valign(time, GTK_ALIGN_START);
  addStyleClass(time, "dim-label");
  gtk_box_pack_end(GTK_BOX(box), time, FALSE, FALSE, 0);
  gtk_container_add(GTK_CONTAINER(row), box);
  return row;
}

static void renderActionLog(GtkRuntime *runtime) {
  clearContainer(runtime->ui.logList);
  const auto entries =
      filteredActionLogEntries(runtime->actionLog, runtime->logFilter);
  for (const auto *entry : entries) {
    auto *row = createActionLogRow(*entry);
    gtk_list_box_insert(GTK_LIST_BOX(runtime->ui.logList), row, -1);
  }
  gtk_widget_show_all(runtime->ui.logList);
  const auto label = formatUiMessage(localizedMessage(
      "Action Log ({0})",
      {std::to_string(runtime->actionLog.entries.size())}));
  gtk_label_set_text(GTK_LABEL(runtime->ui.logToggleLabel), label.c_str());
  gtk_widget_set_sensitive(runtime->ui.logCopyButton, !entries.empty());
  gtk_widget_set_sensitive(runtime->ui.logClearButton,
                           !runtime->actionLog.entries.empty());
}

static void revealActionLog(GtkRuntime *runtime) {
  gtk_toggle_button_set_active(
      GTK_TOGGLE_BUTTON(runtime->ui.logToggleButton), TRUE);
  setLogDrawerVisible(runtime->ui, true);
}

static void appendCompletedAction(
    GtkRuntime *runtime, ActionLogSeverity severity,
    ActionLogCategory category, bool success, UiMessage summary,
    UiMessage detail) {
  appendAction(runtime->actionLog, currentUnixMilliseconds(), severity,
               category,
               success ? ActionLogState::success
                       : ActionLogState::failure,
               std::move(summary), std::move(detail));
  if (severity == ActionLogSeverity::error) {
    revealActionLog(runtime);
  }
}

static UiMessage settingsOperationName(SettingsOperation operation) {
  switch (operation) {
  case SettingsOperation::rate:
    return localizedMessage("Changing sample-rate policy", {});
  case SettingsOperation::dspBackend:
    return localizedMessage("Changing DSP backend", {});
  case SettingsOperation::processing:
    return localizedMessage("Changing processing mode", {});
  case SettingsOperation::none:
    return localizedMessage("Updating settings", {});
  }
  return localizedMessage("Updating settings", {});
}

static UiMessage settingsOperationSuccess(SettingsOperation operation) {
  switch (operation) {
  case SettingsOperation::rate:
    return localizedMessage("Sample-rate policy changed", {});
  case SettingsOperation::dspBackend:
    return localizedMessage("DSP backend changed", {});
  case SettingsOperation::processing:
    return localizedMessage("Processing mode changed", {});
  case SettingsOperation::none:
    return localizedMessage("Settings updated", {});
  }
  return localizedMessage("Settings updated", {});
}

static UiMessage settingsOperationFailure(SettingsOperation operation) {
  switch (operation) {
  case SettingsOperation::rate:
    return localizedMessage("Changing sample-rate policy failed", {});
  case SettingsOperation::dspBackend:
    return localizedMessage("Changing DSP backend failed", {});
  case SettingsOperation::processing:
    return localizedMessage("Changing processing mode failed", {});
  case SettingsOperation::none:
    return localizedMessage("Updating settings failed", {});
  }
  return localizedMessage("Updating settings failed", {});
}

static std::string transactionStateText(const GtkRuntime &runtime) {
  if (!runtime.dialogActive) {
    return {};
  }
  if (!runtime.transactionReady) {
    return translate("Waiting for live PipeTune state…");
  }
  const auto &transaction = runtime.transaction;
  if (!transaction.connected) {
    return translate("Disconnected · settings are read-only");
  }
  if (transaction.conflict) {
    return translate(
        "Live settings changed elsewhere · reopen this dialog");
  }
  if (transaction.liveChangeFailed) {
    return translate("Live preview failed · adjust a setting to retry");
  }
  if (transaction.cancelRequested) {
    return transaction.inFlight == SettingsOperation::none
               ? translate("Finishing rollback…")
               : translate("Restoring the previous live settings…");
  }
  if (transaction.inFlight != SettingsOperation::none) {
    return formatUiMessage(
               settingsOperationName(transaction.inFlight)) +
           "…";
  }
  if (settingsTransactionIsDirty(transaction)) {
    return translate("Live preview active · changes are not saved");
  }
  return translate("Live settings match the saved configuration");
}

static bool controlsAreEditable(const GtkRuntime &runtime) {
  return runtime.dialogActive && runtime.startupConfigAvailable &&
         runtime.transactionReady &&
         runtime.transaction.connected &&
         !runtime.transaction.cancelRequested &&
         !runtime.transaction.conflict;
}

static void renderPresetControls(GtkRuntime *runtime) {
  const auto &settings = runtime->transactionReady
                             ? runtime->transaction.desiredLive
                             : runtime->savedConfig;
  gtk_switch_set_active(
      GTK_SWITCH(runtime->ui.processingEnabledSwitch),
      settings.presetFound ? TRUE : FALSE);
}

static void renderRateControls(GtkRuntime *runtime) {
  const auto &settings = runtime->transactionReady
                             ? runtime->transaction.desiredLive
                             : runtime->savedConfig;
  const auto presentation =
      makeRateSelectionPresentation(runtime->state, settings.ratePolicy);
  runtime->rateChoices = presentation.choices;
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateCombo));
  for (const auto &choice : runtime->rateChoices) {
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(runtime->ui.rateCombo), choice.label.c_str());
  }
  gtk_combo_box_set_active(
      GTK_COMBO_BOX(runtime->ui.rateCombo),
      static_cast<gint>(presentation.activeRateIndex));
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo));
  gtk_combo_box_text_append_text(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo),
      translate("Suggest — let PipeWire choose"));
  gtk_combo_box_text_append_text(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo),
      translate("Force — request the fixed graph rate"));
  gtk_combo_box_set_active(
      GTK_COMBO_BOX(runtime->ui.rateEnforcementCombo),
      static_cast<gint>(presentation.activeEnforcementIndex));
}

static void renderDspControls(GtkRuntime *runtime) {
  const auto &settings = runtime->transactionReady
                             ? runtime->transaction.desiredLive
                             : runtime->savedConfig;
  const auto backend = makeDspBackendSelectionPresentation(
      runtime->state, settings.dspBackend, settings.dspSimdVariant);
  runtime->dspBackendChoices = backend.choices;
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.dspBackendCombo));
  for (const auto &choice : runtime->dspBackendChoices) {
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(runtime->ui.dspBackendCombo),
        choice.label.c_str());
  }
  gtk_combo_box_set_active(
      GTK_COMBO_BOX(runtime->ui.dspBackendCombo),
      static_cast<gint>(backend.activeIndex));
}

static gint uiLanguageComboIndex(UiLanguage language) noexcept {
  switch (language) {
  case UiLanguage::system:
    return 0;
  case UiLanguage::english:
    return 1;
  case UiLanguage::japanese:
    return 2;
  }
  return 0;
}

static bool uiLanguageFromComboIndex(gint index,
                                     UiLanguage *language) noexcept {
  if (language == nullptr) {
    return false;
  }
  switch (index) {
  case 0:
    *language = UiLanguage::system;
    return true;
  case 1:
    *language = UiLanguage::english;
    return true;
  case 2:
    *language = UiLanguage::japanese;
    return true;
  default:
    return false;
  }
}

static void renderLanguageControl(GtkRuntime *runtime) {
  gtk_combo_box_set_active(
      GTK_COMBO_BOX(runtime->ui.languageCombo),
      uiLanguageComboIndex(runtime->uiLanguage));
  if (runtime->languageRestartRequired) {
    gtk_widget_show(runtime->ui.languageRestartNotice);
  } else {
    gtk_widget_hide(runtime->ui.languageRestartNotice);
  }
}

static void renderSettingsControls(GtkRuntime *runtime) {
  runtime->updatingControls = true;
  renderPresetControls(runtime);
  renderRateControls(runtime);
  renderDspControls(runtime);
  renderLanguageControl(runtime);
  runtime->updatingControls = false;
  const auto editable = controlsAreEditable(*runtime);
  gtk_widget_set_sensitive(runtime->ui.processingEnabledSwitch, editable);
  gtk_widget_set_sensitive(runtime->ui.presetCombo, editable &&
                              !runtime->presetChoices.empty());
  gtk_widget_set_sensitive(runtime->ui.presetChooser, editable);
  gtk_widget_set_sensitive(runtime->ui.rateCombo, editable);
  const auto &rateSettings = runtime->transactionReady
                                 ? runtime->transaction.desiredLive
                                 : runtime->savedConfig;
  gtk_widget_set_sensitive(
      runtime->ui.rateEnforcementCombo,
      editable && rateSettings.ratePolicy.mode ==
                      pipetune::SampleRateMode::fixed);
  gtk_widget_set_sensitive(runtime->ui.dspBackendCombo, editable);
  gtk_widget_set_sensitive(runtime->ui.restoreDefaultsButton,
                           runtime->dialogActive);
  gtk_widget_set_sensitive(
      runtime->ui.applyButton,
      runtime->transactionReady &&
          settingsTransactionCanApply(runtime->transaction));
  gtk_widget_set_sensitive(runtime->ui.cancelButton,
                           runtime->dialogActive);
  const auto transactionText = transactionStateText(*runtime);
  gtk_label_set_text(GTK_LABEL(runtime->ui.transactionStateLabel),
                     transactionText.c_str());
}

static void renderStatusArtwork(GtkRuntime *runtime) {
  const auto iconPresentation = statusIconPresentation(runtime->state);
  auto *statusIcon =
      iconPresentation.colorMode == TrayIconColorMode::color
          ? runtime->statusColorIcon
          : runtime->statusGrayscaleIcon;
  gtk_image_set_from_pixbuf(GTK_IMAGE(runtime->ui.statusImage), statusIcon);
  const auto *badge = badgeIconName(iconPresentation.badge);
  if (badge == nullptr) {
    gtk_widget_hide(runtime->ui.statusBadge);
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(runtime->ui.statusBadge), badge,
                                 GTK_ICON_SIZE_MENU);
    gtk_widget_show(runtime->ui.statusBadge);
  }
  const auto summary = connectionSummary(runtime->state);
  gtk_label_set_text(GTK_LABEL(runtime->ui.connectionSummaryLabel),
                     summary.c_str());
  updateTrayBackend(runtime->trayBackend,
                    iconStateForApplication(runtime->state),
                    iconPresentation.colorMode,
                    trayTooltip(runtime->state));
}

static void render(GtkRuntime *runtime) {
  if (runtime == nullptr || runtime->ui.window == nullptr) {
    return;
  }
  renderStatusArtwork(runtime);
  renderStatusRows(runtime);
  renderSettingsControls(runtime);
  renderActionLog(runtime);
}

static void hideAfterRollback(GtkRuntime *runtime) {
  runtime->closeAfterRollback = false;
  runtime->dialogActive = false;
  runtime->transactionReady = false;
  gtk_widget_hide(runtime->ui.window);
  if (runtime->quitAfterRollback) {
    runtime->quitAfterRollback = false;
    requestQuit(runtime);
  }
}

static void finishRollbackIfReady(GtkRuntime *runtime) {
  if (runtime->closeAfterRollback && runtime->transactionReady &&
      settingsTransactionShouldClose(runtime->transaction)) {
    hideAfterRollback(runtime);
  }
}

static void beginRollbackAndClose(GtkRuntime *runtime,
                                  bool quitWhenComplete) {
  if (runtime == nullptr || !runtime->dialogActive) {
    return;
  }
  runtime->closeAfterRollback = true;
  runtime->quitAfterRollback = quitWhenComplete;
  if (!runtime->transactionReady) {
    hideAfterRollback(runtime);
    return;
  }
  requestSettingsCancel(runtime->transaction);
  appendCompletedAction(runtime, ActionLogSeverity::info,
                        ActionLogCategory::settings, true,
                        localizedMessage("Rollback requested", {}),
                        localizedMessage(
                            "Restoring the live settings captured when the "
                            "dialog opened",
                            {}));
  render(runtime);
  driveSettings(runtime);
  finishRollbackIfReady(runtime);
}

static gboolean onWindowDelete(GtkWidget *, GdkEvent *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->quitting) {
    return FALSE;
  }
  beginRollbackAndClose(
      runtime,
      runtime->trayAvailability !=
          TrayBackendAvailabilityState::available);
  return TRUE;
}

static gboolean onWindowKeyPress(GtkWidget *, GdkEventKey *event,
                                 gpointer userData) {
  const auto escape = event->keyval == GDK_KEY_Escape;
  const auto altF4 = event->keyval == GDK_KEY_F4 &&
                     (event->state & GDK_MOD1_MASK) != 0;
  if (!escape && !altF4) {
    return FALSE;
  }
  beginRollbackAndClose(static_cast<GtkRuntime *>(userData), false);
  return TRUE;
}

static void onWindowDestroy(GtkWidget *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  runtime->ui.window = nullptr;
}

static void onCancelClicked(GtkButton *, gpointer userData) {
  beginRollbackAndClose(static_cast<GtkRuntime *>(userData), false);
}

static void onCloseClicked(GtkButton *, gpointer userData) {
  beginRollbackAndClose(static_cast<GtkRuntime *>(userData), false);
}

static void editDesiredSettings(
    GtkRuntime *runtime, const pipetune::StartupConfig &desired) {
  if (!controlsAreEditable(*runtime)) {
    return;
  }
  editSettingsTransaction(runtime->transaction, desired);
  render(runtime);
  driveSettings(runtime);
}

static void onRateChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >= runtime->rateChoices.size()) {
    return;
  }
  const auto &choice =
      runtime->rateChoices[static_cast<std::size_t>(selected)];
  auto desired = runtime->transaction.desiredLive;
  desired.ratePolicy.mode = choice.mode;
  desired.ratePolicy.fixedRate = choice.fixedRate;
  if (choice.mode == pipetune::SampleRateMode::automatic) {
    desired.ratePolicy.enforcement =
        pipetune::SampleRateEnforcement::suggest;
  }
  editDesiredSettings(runtime, desired);
}

static void onRateEnforcementChanged(GtkComboBox *combo,
                                     gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected != 0 && selected != 1) {
    return;
  }
  auto desired = runtime->transaction.desiredLive;
  desired.ratePolicy.enforcement =
      selected == 1 ? pipetune::SampleRateEnforcement::force
                    : pipetune::SampleRateEnforcement::suggest;
  editDesiredSettings(runtime, desired);
}

static void onDspBackendChanged(GtkComboBox *combo,
                                gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >=
          runtime->dspBackendChoices.size()) {
    return;
  }
  const auto &choice =
      runtime->dspBackendChoices[static_cast<std::size_t>(selected)];
  auto desired = runtime->transaction.desiredLive;
  desired.dspBackend = choice.kind;
  desired.dspSimdVariant = choice.simdVariant;
  editDesiredSettings(runtime, desired);
}

static void onLanguageChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls) {
    return;
  }
  auto selected = UiLanguage::system;
  if (!uiLanguageFromComboIndex(
          gtk_combo_box_get_active(combo), &selected) ||
      selected == runtime->uiLanguage) {
    return;
  }
  const auto saved = saveUiLanguagePreference(
      runtime->uiLanguageConfigPath, selected);
  if (!saved.error.empty()) {
    runtime->updatingControls = true;
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(runtime->ui.languageCombo),
        uiLanguageComboIndex(runtime->uiLanguage));
    runtime->updatingControls = false;
    appendCompletedAction(
        runtime, ActionLogSeverity::error,
        ActionLogCategory::persistence, false,
        localizedMessage("Cannot save language preference", {}),
        technicalMessage(saved.error));
    render(runtime);
    return;
  }
  runtime->uiLanguage = selected;
  runtime->languageRestartRequired =
      runtime->uiLanguage != runtime->presentationLanguage;
  appendCompletedAction(
      runtime, ActionLogSeverity::info,
      ActionLogCategory::persistence, true,
      localizedMessage("Language preference saved", {}),
      technicalMessage(runtime->uiLanguageConfigPath.string()));
  render(runtime);
}

static void onProcessingActiveChanged(GObject *object, GParamSpec *,
                                      gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  const auto active =
      gtk_switch_get_active(GTK_SWITCH(object)) != FALSE;
  auto desired = runtime->transaction.desiredLive;
  if (!active) {
    if (desired.presetFound) {
      runtime->lastPresetPath = desired.presetPath;
    }
    desired.presetFound = false;
    desired.presetPath.clear();
    editDesiredSettings(runtime, desired);
    return;
  }
  if (runtime->lastPresetPath.empty()) {
    runtime->updatingControls = true;
    gtk_switch_set_active(GTK_SWITCH(object), FALSE);
    runtime->updatingControls = false;
    appendCompletedAction(
        runtime, ActionLogSeverity::warning, ActionLogCategory::settings,
        false, localizedMessage("Preset required", {}),
        localizedMessage(
            "Choose a preset before enabling DSP processing", {}));
    render(runtime);
    return;
  }
  desired.presetFound = true;
  desired.presetPath = runtime->lastPresetPath;
  editDesiredSettings(runtime, desired);
}

static std::string presetChoiceLabel(const PresetChoice &choice) {
  if (choice.source == PresetSource::saved) {
    return formatUiMessage(localizedMessage(
        "Saved in EffeTune · {0}", {choice.name}));
  }
  return formatUiMessage(localizedMessage(
      "Standard · {0} · {1}", {choice.category, choice.name}));
}

static std::string catalogDiagnosticText(
    const std::vector<std::string> &diagnostics,
    std::string_view pathResolutionError) {
  auto text = std::string{};
  appendDetail(text, pathResolutionError);
  for (const auto &diagnostic : diagnostics) {
    appendDetail(text, diagnostic);
  }
  return text;
}

static void replacePresetChoices(
    GtkRuntime *runtime, std::vector<PresetChoice> choices) {
  runtime->presetChoices = std::move(choices);
  runtime->updatingControls = true;
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo));
  gtk_combo_box_text_append_text(
      GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo),
      translate("Choose a standard or saved EffeTune preset…"));
  auto activeIndex = gint{0};
  for (auto index = std::size_t{0}; index < runtime->presetChoices.size();
       ++index) {
    const auto &choice = runtime->presetChoices[index];
    const auto label = presetChoiceLabel(choice);
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo), label.c_str());
    if (choice.source == PresetSource::standard &&
        choice.path == runtime->lastPresetPath) {
      activeIndex = static_cast<gint>(index + 1);
    }
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(runtime->ui.presetCombo),
                           activeIndex);
  runtime->updatingControls = false;
}

static void refreshSavedPresetCatalog(GtkRuntime *runtime) {
  if (runtime->effetuneUserPresetPath.empty()) {
    return;
  }
  const auto refresh =
      loadEffeTuneSavedPresets(runtime->effetuneUserPresetPath);
  auto choices = applyEffeTuneSavedPresetRefresh(
      runtime->presetChoices, refresh);
  runtime->presetCatalogSavedDiagnostic =
      catalogDiagnosticText(refresh.diagnostics, {});
  replacePresetChoices(runtime, std::move(choices));
}

static void onEffeTunePresetFileChanged(void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->shuttingDown) {
    return;
  }
  refreshSavedPresetCatalog(runtime);
  render(runtime);
}

static void initializePresetCatalog(GtkRuntime *runtime) {
  const auto standard = loadEffeTunePresetCatalog(
      kEffeTuneStandardPresetDirectory, {});
  runtime->presetCatalogSourceDiagnostic =
      catalogDiagnosticText(standard.diagnostics, {});
  replacePresetChoices(runtime, standard.choices);

  const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
  const auto *home = std::getenv("HOME");
  const auto userPath = resolveEffeTuneUserPresetPath(
      xdgConfigHome == nullptr ? std::string_view{}
                               : std::string_view(xdgConfigHome),
      home == nullptr ? std::filesystem::path{}
                      : std::filesystem::path(home));
  appendDetail(runtime->presetCatalogSourceDiagnostic, userPath.error);
  if (userPath.error.empty()) {
    runtime->effetuneUserPresetPath = userPath.path;
    refreshSavedPresetCatalog(runtime);
    const auto monitor = createEffeTunePresetFileMonitor(
        runtime->effetuneUserPresetPath,
        onEffeTunePresetFileChanged, runtime);
    runtime->presetFileMonitor = monitor.monitor;
    appendDetail(runtime->presetCatalogSourceDiagnostic, monitor.error);
  }
  auto diagnostics = runtime->presetCatalogSourceDiagnostic;
  appendDetail(diagnostics, runtime->presetCatalogSavedDiagnostic);
  if (!diagnostics.empty()) {
    appendCompletedAction(
        runtime, ActionLogSeverity::warning,
        ActionLogCategory::application, false,
        localizedMessage("Some preset sources are unavailable", {}),
        technicalMessage(diagnostics));
  }
}

static void selectPresetPath(GtkRuntime *runtime,
                             const std::filesystem::path &path) {
  if (path.empty()) {
    return;
  }
  runtime->lastPresetPath = path;
  auto desired = runtime->transaction.desiredLive;
  desired.presetFound = true;
  desired.presetPath = path;
  editDesiredSettings(runtime, desired);
}

static void onPresetComboChanged(GtkComboBox *combo,
                                 gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  const auto active = gtk_combo_box_get_active(combo);
  if (active <= 0 ||
      static_cast<std::size_t>(active - 1) >=
          runtime->presetChoices.size()) {
    return;
  }
  const auto &choice =
      runtime->presetChoices[static_cast<std::size_t>(active - 1)];
  const auto resolved = resolvePresetChoicePath(
      choice,
      runtime->startupConfigPath.parent_path() / "effetune-presets");
  if (!resolved.error.empty()) {
    appendCompletedAction(
        runtime, ActionLogSeverity::error, ActionLogCategory::settings,
        false, localizedMessage("Cannot prepare preset", {}),
        technicalMessage(resolved.error));
    render(runtime);
    return;
  }
  runtime->updatingControls = true;
  gtk_file_chooser_set_filename(
      GTK_FILE_CHOOSER(runtime->ui.presetChooser),
      resolved.path.c_str());
  runtime->updatingControls = false;
  selectPresetPath(runtime, resolved.path);
}

static void onPresetFileSet(GtkFileChooserButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  auto *filename = gtk_file_chooser_get_filename(
      GTK_FILE_CHOOSER(runtime->ui.presetChooser));
  if (filename == nullptr) {
    return;
  }
  auto error = std::error_code{};
  auto path =
      std::filesystem::absolute(filename, error).lexically_normal();
  g_free(filename);
  if (error) {
    appendCompletedAction(
        runtime, ActionLogSeverity::error, ActionLogCategory::settings,
        false, localizedMessage("Cannot resolve preset", {}),
        technicalMessage(error.message()));
    render(runtime);
    return;
  }
  runtime->updatingControls = true;
  gtk_combo_box_set_active(GTK_COMBO_BOX(runtime->ui.presetCombo), 0);
  runtime->updatingControls = false;
  selectPresetPath(runtime, path);
}

static void onRestoreDefaultsClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime == nullptr || !runtime->dialogActive) {
    return;
  }
  const auto defaults = defaultStartupConfig();
  if (!runtime->startupConfigAvailable &&
      !runtime->startupConfigPath.empty()) {
    const auto error = pipetune::saveStartupConfig(
        runtime->startupConfigPath, defaults);
    if (!error.empty()) {
      appendCompletedAction(
          runtime, ActionLogSeverity::error,
          ActionLogCategory::persistence, false,
          localizedMessage("Cannot save settings", {}),
          technicalMessage(error));
      revealActionLog(runtime);
    } else {
      runtime->savedConfig = defaults;
      runtime->startupConfigAvailable = true;
    }
  }
  if (!runtime->transactionReady) {
    beginTransactionFromRuntime(runtime);
  }
  if (!runtime->transactionReady) {
    runtime->transaction = beginSettingsTransaction(
        runtime->savedConfig, runtime->savedConfig, 0, false);
    runtime->transactionReady = true;
  }
  appendCompletedAction(
      runtime, ActionLogSeverity::info, ActionLogCategory::settings, true,
      localizedMessage("Defaults selected", {}),
      localizedMessage(
          "Defaults are being applied live; use Apply to save them", {}));
  restoreSettingsDefaults(runtime->transaction, defaults);
  render(runtime);
  driveSettings(runtime);
}

static std::string persistenceTestDiagnostic() {
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
  const auto *guardPath =
      std::getenv("PIPETUNE_GTK_E2E_PERSISTENCE_GUARD");
  if (guardPath != nullptr && guardPath[0] != '\0') {
    auto stream = std::ifstream(guardPath);
    auto value = std::string{};
    std::getline(stream, value);
    if (value == "deny") {
      return "E2E persistence guard rejected the write";
    }
  }
#endif
  return {};
}

static void onApplyClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!runtime->transactionReady ||
      !settingsTransactionCanApply(runtime->transaction)) {
    return;
  }
  const auto pending = appendPendingAction(
      runtime->actionLog, currentUnixMilliseconds(),
      ActionLogCategory::persistence,
      localizedMessage("Saving all settings", {}),
      technicalMessage(runtime->startupConfigPath.string()));
  auto error = persistenceTestDiagnostic();
  if (error.empty()) {
    error = pipetune::saveStartupConfig(
        runtime->startupConfigPath, runtime->transaction.desiredLive);
  }
  const auto success = error.empty();
  completeSettingsPersistence(runtime->transaction, success, error);
  if (success) {
    runtime->savedConfig = runtime->transaction.saved;
    completePendingAction(
        runtime->actionLog, pending, currentUnixMilliseconds(), true,
        ActionLogSeverity::info,
        localizedMessage("All settings saved", {}),
        technicalMessage(runtime->startupConfigPath.string()));
  } else {
    setControlDiagnostic(
        runtime->state,
        "Live settings remain active, but persistence failed: " + error);
    completePendingAction(
        runtime->actionLog, pending, currentUnixMilliseconds(), false,
        ActionLogSeverity::error,
        localizedMessage("Cannot save settings", {}),
        technicalMessage(error));
    revealActionLog(runtime);
  }
  render(runtime);
}

static void onLogToggleChanged(GtkToggleButton *button,
                               gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  setLogDrawerVisible(runtime->ui,
                      gtk_toggle_button_get_active(button) != FALSE);
}

static void onLogFilterChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  const auto active = gtk_combo_box_get_active(combo);
  runtime->logFilter =
      active == 2 ? ActionLogFilter::errors
                  : active == 1 ? ActionLogFilter::warnings
                                : ActionLogFilter::all;
  renderActionLog(runtime);
}

static std::string visibleActionLogText(const GtkRuntime &runtime) {
  const auto entries =
      filteredActionLogEntries(runtime.actionLog, runtime.logFilter);
  auto text = std::string{};
  for (const auto *entry : entries) {
    if (!text.empty()) {
      text.push_back('\n');
    }
    text += formatActionTime(entry->timestampUnixMilliseconds);
    text += "  ";
    text += formatUiMessage(entry->summary);
    if (!uiMessageIsEmpty(entry->detail)) {
      text += " — ";
      text += formatUiMessage(entry->detail);
    }
  }
  return text;
}

static void onLogCopyClicked(GtkButton *, gpointer userData) {
  const auto *runtime = static_cast<GtkRuntime *>(userData);
  const auto text = visibleActionLogText(*runtime);
  auto *clipboard = gtk_clipboard_get(GDK_SELECTION_CLIPBOARD);
  gtk_clipboard_set_text(clipboard, text.c_str(),
                         static_cast<gint>(text.size()));
}

static void onLogClearClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  clearActionLog(runtime->actionLog);
  renderActionLog(runtime);
}

static void onSettingsOperationReply(const ControlClientReply &reply,
                                     void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->shuttingDown || !runtime->transactionReady) {
    return;
  }
  const auto operation = runtime->transaction.inFlight;
  const auto pendingId = runtime->pendingActionId;
  runtime->pendingActionId = 0;
  setControlOperationPending(runtime->state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(runtime->state, reply.transportError);
    markSettingsDisconnected(runtime->transaction, reply.transportError);
    completePendingAction(
        runtime->actionLog, pendingId, currentUnixMilliseconds(), false,
        ActionLogSeverity::error, settingsOperationFailure(operation),
        technicalMessage(reply.transportError));
    revealActionLog(runtime);
    render(runtime);
    scheduleReconnect(runtime);
    return;
  }

  applyControlResponse(runtime->state, reply.response,
                       currentMonotonicMilliseconds());
  const auto success = reply.response.valid && reply.response.success;
  const auto confirmed =
      success ? startupConfigFromRuntime(reply.response.status)
              : runtime->transaction.confirmedLive;
  const auto diagnostic =
      success ? std::string{} : controlDiagnostic(reply);
  completeSettingsOperation(runtime->transaction, success, confirmed,
                            success
                                ? reply.response.status.configurationRevision
                                : runtime->transaction.confirmedRevision,
                            diagnostic);
  completePendingAction(
      runtime->actionLog, pendingId, currentUnixMilliseconds(), success,
      success ? ActionLogSeverity::info : ActionLogSeverity::error,
      success ? settingsOperationSuccess(operation)
              : settingsOperationFailure(operation),
      technicalMessage(diagnostic));
  if (!success) {
    revealActionLog(runtime);
  }
  render(runtime);
  if (success) {
    driveSettings(runtime);
    finishRollbackIfReady(runtime);
  }
}

static void dispatchSettingsOperation(
    GtkRuntime *runtime, SettingsOperation operation) {
  const auto &target = runtime->transaction.inFlightTarget;
  switch (operation) {
  case SettingsOperation::rate:
    setControlRateAsync(runtime->controlClient, target.ratePolicy,
                        onSettingsOperationReply, runtime);
    return;
  case SettingsOperation::dspBackend:
    setControlDspBackendAsync(runtime->controlClient, target.dspBackend,
                              target.dspSimdVariant,
                              onSettingsOperationReply, runtime);
    return;
  case SettingsOperation::processing:
    if (target.presetFound) {
      loadControlPresetAsync(runtime->controlClient, target.presetPath,
                             onSettingsOperationReply, runtime);
    } else {
      bypassControlAsync(runtime->controlClient, onSettingsOperationReply,
                         runtime);
    }
    return;
  case SettingsOperation::none:
    return;
  }
}

static void driveSettings(GtkRuntime *runtime) {
  if (!runtime->transactionReady || runtime->controlClient == nullptr) {
    return;
  }
  finishRollbackIfReady(runtime);
  if (!runtime->transactionReady) {
    return;
  }
  const auto operation = nextSettingsOperation(runtime->transaction);
  if (operation == SettingsOperation::none ||
      !beginSettingsOperation(runtime->transaction, operation)) {
    render(runtime);
    finishRollbackIfReady(runtime);
    return;
  }
  runtime->pendingActionId = appendPendingAction(
      runtime->actionLog, currentUnixMilliseconds(),
      ActionLogCategory::settings, settingsOperationName(operation),
      technicalMessage({}));
  setControlOperationPending(runtime->state, true);
  render(runtime);
  dispatchSettingsOperation(runtime, operation);
}

static void beginTransactionFromRuntime(GtkRuntime *runtime) {
  if (!runtime->dialogActive ||
      runtime->state.connection != ControlConnectionState::connected ||
      !runtime->state.hasRuntimeStatus) {
    return;
  }
  const auto live = startupConfigFromRuntime(runtime->state.runtime);
  runtime->transaction =
      beginSettingsTransaction(
          runtime->savedConfig, live,
          runtime->state.runtime.configurationRevision, true);
  runtime->transactionReady = true;
  if (live.presetFound) {
    runtime->lastPresetPath = live.presetPath;
  } else if (runtime->savedConfig.presetFound) {
    runtime->lastPresetPath = runtime->savedConfig.presetPath;
  }
}

static void onSubscriptionMessage(
    const pipetune::ControlResponseParseResult &message, void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  const auto previouslyConnected =
      runtime->state.connection == ControlConnectionState::connected &&
      runtime->state.hasRuntimeStatus;
  applyControlResponse(runtime->state, message,
                       currentMonotonicMilliseconds());
  if (!previouslyConnected && runtime->state.hasRuntimeStatus) {
    appendCompletedAction(runtime, ActionLogSeverity::info,
                          ActionLogCategory::control, true,
                          localizedMessage("Connected to PipeTune", {}),
                          localizedMessage(
                              "Live status subscription established", {}));
  }
  if (runtime->dialogActive) {
    const auto live = startupConfigFromRuntime(runtime->state.runtime);
    if (!runtime->transactionReady) {
      beginTransactionFromRuntime(runtime);
    } else if (!runtime->transaction.connected) {
      reconnectSettingsTransaction(
          runtime->transaction, live,
          runtime->state.runtime.configurationRevision);
      appendCompletedAction(
          runtime, ActionLogSeverity::info, ActionLogCategory::control,
          true, localizedMessage("PipeTune reconnected", {}),
          localizedMessage(
              "Pending dialog settings will be reapplied", {}));
    } else {
      observeSettingsRuntime(
          runtime->transaction, live,
          runtime->state.runtime.configurationRevision);
      if (runtime->transaction.conflict) {
        appendCompletedAction(
            runtime, ActionLogSeverity::warning,
            ActionLogCategory::settings, false,
            localizedMessage("Live settings changed externally", {}),
            technicalMessage(runtime->transaction.diagnostic));
      }
    }
  }
  render(runtime);
  driveSettings(runtime);
}

static void onConnectionChanged(bool connected, std::string_view error,
                                void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->shuttingDown || connected) {
    return;
  }
  markControlDisconnected(runtime->state, error);
  if (runtime->transactionReady) {
    markSettingsDisconnected(runtime->transaction, error);
  }
  appendCompletedAction(
      runtime, ActionLogSeverity::warning, ActionLogCategory::control,
      false, localizedMessage("PipeTune disconnected", {}),
      technicalMessage(error));
  render(runtime);
  scheduleReconnect(runtime);
}

static gboolean reconnectControl(gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  runtime->reconnectSource = 0;
  if (runtime->shuttingDown || runtime->controlClient == nullptr) {
    return G_SOURCE_REMOVE;
  }
  markControlConnecting(runtime->state);
  render(runtime);
  startControlSubscription(runtime->controlClient);
  return G_SOURCE_REMOVE;
}

static void scheduleReconnect(GtkRuntime *runtime) {
  if (runtime->shuttingDown || runtime->controlClient == nullptr ||
      runtime->reconnectSource != 0) {
    return;
  }
  runtime->reconnectSource = g_timeout_add_seconds(
      kReconnectDelaySeconds, reconnectControl, runtime);
}

static void connectMainWindowSignals(GtkRuntime *runtime) {
  g_signal_connect(runtime->ui.window, "delete-event",
                   G_CALLBACK(onWindowDelete), runtime);
  g_signal_connect(runtime->ui.window, "key-press-event",
                   G_CALLBACK(onWindowKeyPress), runtime);
  g_signal_connect(runtime->ui.window, "destroy",
                   G_CALLBACK(onWindowDestroy), runtime);
  g_signal_connect(runtime->ui.cancelButton, "clicked",
                   G_CALLBACK(onCancelClicked), runtime);
  g_signal_connect(runtime->ui.closeButton, "clicked",
                   G_CALLBACK(onCloseClicked), runtime);
  g_signal_connect(runtime->ui.applyButton, "clicked",
                   G_CALLBACK(onApplyClicked), runtime);
  g_signal_connect(runtime->ui.rateCombo, "changed",
                   G_CALLBACK(onRateChanged), runtime);
  g_signal_connect(runtime->ui.rateEnforcementCombo, "changed",
                   G_CALLBACK(onRateEnforcementChanged), runtime);
  g_signal_connect(runtime->ui.dspBackendCombo, "changed",
                   G_CALLBACK(onDspBackendChanged), runtime);
  g_signal_connect(runtime->ui.languageCombo, "changed",
                   G_CALLBACK(onLanguageChanged), runtime);
  g_signal_connect(runtime->ui.processingEnabledSwitch, "notify::active",
                   G_CALLBACK(onProcessingActiveChanged), runtime);
  g_signal_connect(runtime->ui.presetCombo, "changed",
                   G_CALLBACK(onPresetComboChanged), runtime);
  g_signal_connect(runtime->ui.presetChooser, "file-set",
                   G_CALLBACK(onPresetFileSet), runtime);
  g_signal_connect(runtime->ui.restoreDefaultsButton, "clicked",
                   G_CALLBACK(onRestoreDefaultsClicked), runtime);
  g_signal_connect(runtime->ui.logToggleButton, "toggled",
                   G_CALLBACK(onLogToggleChanged), runtime);
  g_signal_connect(runtime->ui.logFilterCombo, "changed",
                   G_CALLBACK(onLogFilterChanged), runtime);
  g_signal_connect(runtime->ui.logCopyButton, "clicked",
                   G_CALLBACK(onLogCopyClicked), runtime);
  g_signal_connect(runtime->ui.logClearButton, "clicked",
                   G_CALLBACK(onLogClearClicked), runtime);
}

static void createMainWindowPresentation(GtkRuntime *runtime) {
  runtime->ui = createMainWindowUi(
      runtime->application, pipetune::version(),
      pipetune::effetuneVersion());
  connectMainWindowSignals(runtime);
}

static void destroyMainWindowPresentation(GtkRuntime *runtime) noexcept {
  runtime->statusRows.clear();
  runtime->statusLoadMeter = {};
  destroyMainWindowUi(runtime->ui);
}

static void presentWindow(GtkRuntime *runtime,
                          guint32 userInteractionTime) {
  if (runtime == nullptr || runtime->ui.window == nullptr) {
    return;
  }
  runtime->dialogActive = true;
  runtime->closeAfterRollback = false;
  runtime->quitAfterRollback = false;
  refreshSavedPresetCatalog(runtime);
  if (!runtime->transactionReady) {
    beginTransactionFromRuntime(runtime);
  }
  render(runtime);
  presentMainWindow(runtime->ui, userInteractionTime);
}

static void releaseApplicationHold(GtkRuntime *runtime) {
  if (runtime->applicationHeld) {
    g_application_release(G_APPLICATION(runtime->application));
    runtime->applicationHeld = false;
  }
}

static void requestQuit(GtkRuntime *runtime) {
  if (runtime == nullptr || runtime->quitting) {
    return;
  }
  runtime->quitting = true;
  releaseApplicationHold(runtime);
  g_application_quit(G_APPLICATION(runtime->application));
}

static void onTrayAvailabilityChanged(
    GtkRuntime *runtime, TrayBackendAvailabilityState availability) {
  if (!runtime->shuttingDown) {
    runtime->trayAvailability = availability;
  }
}

static void initializeStartupConfig(GtkRuntime *runtime) {
  const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
  const auto *home = std::getenv("HOME");
  const auto resolved = pipetune::resolveStartupConfigPath(
      xdgConfigHome == nullptr ? std::string_view{}
                               : std::string_view(xdgConfigHome),
      home == nullptr ? std::filesystem::path{}
                      : std::filesystem::path(home));
  if (!resolved.error.empty()) {
    setControlDiagnostic(runtime->state, resolved.error);
    appendCompletedAction(
        runtime, ActionLogSeverity::error,
        ActionLogCategory::persistence, false,
        localizedMessage(
            "Startup configuration path unavailable", {}),
        technicalMessage(resolved.error));
    return;
  }
  runtime->startupConfigPath = resolved.path;
  const auto loaded =
      pipetune::loadStartupConfig(runtime->startupConfigPath);
  if (!loaded.error.empty()) {
    setControlDiagnostic(runtime->state, loaded.error);
    appendCompletedAction(
        runtime, ActionLogSeverity::error,
        ActionLogCategory::persistence, false,
        localizedMessage("Cannot load startup configuration", {}),
        technicalMessage(loaded.error));
    return;
  }
  runtime->savedConfig = loaded.config;
  runtime->startupConfigAvailable = true;
  if (loaded.config.presetFound) {
    runtime->lastPresetPath = loaded.config.presetPath;
  }
}

static void appendLocalizationWarnings(GtkRuntime *runtime) {
  if (!runtime->uiLanguageLoadWarning.empty()) {
    appendCompletedAction(
        runtime, ActionLogSeverity::warning,
        ActionLogCategory::persistence, false,
        localizedMessage("Cannot use saved language preference", {}),
        technicalMessage(runtime->uiLanguageLoadWarning));
  }
  if (!runtime->localizationWarning.empty()) {
    appendCompletedAction(
        runtime, ActionLogSeverity::warning,
        ActionLogCategory::application, false,
        localizedMessage("Cannot apply UI language", {}),
        technicalMessage(runtime->localizationWarning));
  }
}

static void initializeControlClient(GtkRuntime *runtime) {
  const auto socket = pipetune::resolveControlSocketPath({});
  if (!socket.error.empty()) {
    markControlDisconnected(runtime->state, socket.error);
    appendCompletedAction(
        runtime, ActionLogSeverity::error, ActionLogCategory::control,
        false, localizedMessage("Control socket unavailable", {}),
        technicalMessage(socket.error));
    return;
  }
  runtime->controlClient = createControlClient(
      socket.path,
      {.message = onSubscriptionMessage,
       .connectionChanged = onConnectionChanged,
       .userData = runtime});
  markControlConnecting(runtime->state);
  appendCompletedAction(runtime, ActionLogSeverity::info,
                        ActionLogCategory::control, true,
                        localizedMessage("Connecting to PipeTune", {}),
                        technicalMessage(socket.path.string()));
  startControlSubscription(runtime->controlClient);
}

static void onApplicationStartup(GApplication *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  createMainWindowPresentation(runtime);
  initializeStatusArtwork(runtime);
  appendLocalizationWarnings(runtime);
  initializeStartupConfig(runtime);
  initializeStatusRows(runtime);
  initializePresetCatalog(runtime);
  g_application_hold(G_APPLICATION(runtime->application));
  runtime->applicationHeld = true;
  runtime->trayBackend = createTrayBackend({
      .application = G_APPLICATION(runtime->application),
      .identifier = "pipetune",
      .title = "PipeTune",
      .iconState = TrayIconState::disconnected,
      .colorMode = TrayIconColorMode::grayscale,
      .tooltip = translate("PipeTune: disconnected"),
      .callbacks =
          {
              .activate =
                  [runtime](std::uint32_t userInteractionTime) {
                    presentWindow(runtime, userInteractionTime);
                  },
              .quit = [runtime]() {
                if (runtime->dialogActive) {
                  beginRollbackAndClose(runtime, true);
                } else {
                  requestQuit(runtime);
                }
              },
              .availabilityChanged =
                  [runtime](TrayBackendAvailabilityState availability) {
                    onTrayAvailabilityChanged(runtime, availability);
                  },
          },
  });
  initializeControlClient(runtime);
  render(runtime);
}

static gint onApplicationCommandLine(GApplication *,
                                     GApplicationCommandLine *commandLine,
                                     gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  auto argumentCount = int{0};
  auto **arguments =
      g_application_command_line_get_arguments(commandLine, &argumentCount);
  auto views = std::vector<std::string_view>{};
  views.reserve(argumentCount > 1
                    ? static_cast<std::size_t>(argumentCount - 1)
                    : 0);
  for (auto index = int{1}; index < argumentCount; ++index) {
    views.emplace_back(arguments[index]);
  }
  const auto parsed = parseLaunchOptions(views);
  g_strfreev(arguments);
  if (!parsed.error.empty()) {
    g_application_command_line_printerr(commandLine, "pipetune-gtk: %s\n",
                                        parsed.error.c_str());
    return 2;
  }
  if (parsed.options.action == LaunchAction::quit) {
    if (runtime->dialogActive) {
      beginRollbackAndClose(runtime, true);
    } else {
      requestQuit(runtime);
    }
    return 0;
  }
  if (!runtime->activationHandled) {
    runtime->activationHandled = true;
    if (!parsed.options.hidden) {
      presentWindow(runtime, GDK_CURRENT_TIME);
    }
    return 0;
  }
  if (!parsed.options.hidden) {
    presentWindow(runtime, GDK_CURRENT_TIME);
  }
  return 0;
}

static void onApplicationShutdown(GApplication *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  runtime->shuttingDown = true;
  if (runtime->reconnectSource != 0) {
    g_source_remove(runtime->reconnectSource);
    runtime->reconnectSource = 0;
  }
  destroyEffeTunePresetFileMonitor(runtime->presetFileMonitor);
  runtime->presetFileMonitor = nullptr;
  destroyControlClient(runtime->controlClient);
  runtime->controlClient = nullptr;
  destroyTrayBackend(runtime->trayBackend);
  runtime->trayBackend = nullptr;
  destroyMainWindowPresentation(runtime);
  releaseStatusArtwork(runtime);
  releaseApplicationHold(runtime);
}

static int runApplication(int argc, char **argv) {
  const auto originalLocalization = captureUiLocalizationEnvironment();
  const auto *xdgConfigHome = std::getenv("XDG_CONFIG_HOME");
  const auto *home = std::getenv("HOME");
  const auto uiLanguageConfigPath = resolveUiLanguageConfigPath(
      xdgConfigHome == nullptr ? std::string_view{}
                               : std::string_view(xdgConfigHome),
      home == nullptr ? std::string_view{} : std::string_view(home));
  const auto loadedLanguage =
      loadUiLanguagePreference(uiLanguageConfigPath);
  auto presentationLanguage = loadedLanguage.language;
  auto localization = applyUiLanguage(
      originalLocalization, presentationLanguage, kGtkLocaleDirectory);
  auto localizationWarning = localization.warning;
  if (!localization.warning.empty() &&
      presentationLanguage != UiLanguage::system) {
    presentationLanguage = UiLanguage::system;
    localization = applyUiLanguage(
        originalLocalization, presentationLanguage, kGtkLocaleDirectory);
    appendDetail(localizationWarning, localization.warning);
  }
  gtk_disable_setlocale();
  auto *application = gtk_application_new(
      applicationId(), G_APPLICATION_HANDLES_COMMAND_LINE);
  auto runtime = GtkRuntime{
      .application = application,
      .state = initialApplicationState(),
      .controlClient = nullptr,
      .trayBackend = nullptr,
      .trayAvailability = TrayBackendAvailabilityState::pending,
      .startupConfigPath = {},
      .savedConfig = defaultStartupConfig(),
      .startupConfigAvailable = false,
      .originalLocalization = originalLocalization,
      .uiLanguageConfigPath = uiLanguageConfigPath,
      .presentationLanguage = presentationLanguage,
      .uiLanguage = presentationLanguage,
      .languageRestartRequired = false,
      .uiLanguageLoadWarning = loadedLanguage.warning,
      .localizationWarning = localizationWarning,
      .transaction = {},
      .transactionReady = false,
      .dialogActive = false,
      .updatingControls = false,
      .closeAfterRollback = false,
      .quitAfterRollback = false,
      .lastPresetPath = {},
      .presetChoices = {},
      .effetuneUserPresetPath = {},
      .presetFileMonitor = nullptr,
      .presetCatalogSourceDiagnostic = {},
      .presetCatalogSavedDiagnostic = {},
      .rateChoices = {},
      .dspBackendChoices = {},
      .statusRows = {},
      .statusLoadMeter = {},
      .actionLog = createActionLog(kActionLogCapacity),
      .logFilter = ActionLogFilter::all,
      .pendingActionId = 0,
      .reconnectSource = 0,
      .applicationHeld = false,
      .activationHandled = false,
      .shuttingDown = false,
      .quitting = false,
      .ui = {},
      .statusColorIcon = nullptr,
      .statusGrayscaleIcon = nullptr,
  };
  g_signal_connect(application, "startup",
                   G_CALLBACK(onApplicationStartup), &runtime);
  g_signal_connect(application, "command-line",
                   G_CALLBACK(onApplicationCommandLine), &runtime);
  g_signal_connect(application, "shutdown",
                   G_CALLBACK(onApplicationShutdown), &runtime);
  const auto result =
      g_application_run(G_APPLICATION(application), argc, argv);
  g_object_unref(application);
  restoreUiLocalizationEnvironment(originalLocalization);
  return result;
}

} // namespace pipetune_gtk

int main(int argc, char **argv) {
  auto arguments = std::vector<std::string_view>{};
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (auto index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  const auto parsed = pipetune_gtk::parseLaunchOptions(arguments);
  if (!parsed.error.empty()) {
    std::cerr << "pipetune-gtk: " << parsed.error << "\n\n"
              << pipetune_gtk::launchOptionsUsage();
    return 2;
  }
  if (parsed.options.action == pipetune_gtk::LaunchAction::help) {
    std::cout << pipetune_gtk::launchOptionsUsage();
    return 0;
  }
  if (parsed.options.action == pipetune_gtk::LaunchAction::version) {
    std::cout << pipetune_gtk::versionText() << '\n';
    return 0;
  }
  return pipetune_gtk::runApplication(argc, argv);
}
