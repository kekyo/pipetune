/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "action-log.h"
#include "application-state.h"
#include "control-client.h"
#include "dsp-backend-selection-model.h"
#include "installed-locales.h"
#include "installed-presets.h"
#include "installed-tools.h"
#include "launch-options.h"
#include "localization.h"
#include "main-window.h"
#include "output-mapping-model.h"
#include "status-text.h"
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
#include "user-setup-client.h"

#include "pipetune/control_socket.h"
#include "pipetune/dsp_idle.h"
#include "pipetune/startup_config.h"
#include "pipetune/version.h"

#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
#include <gestament/gtk.h>
#endif

#include <gtk/gtk.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <unistd.h>

namespace pipetune_gtk {

constexpr auto kReconnectDelaySeconds = guint{2};
constexpr auto kStatusArtworkSize = int{48};
constexpr auto kActionLogCapacity = std::size_t{500};
constexpr auto kDspLoadStatusId = std::string_view{"dsp.load"};

struct StatusRowWidgets {
  GtkWidget *text;
};

struct OutputChannelWidgets {
  GtkWidget *physical;
  GtkWidget *number;
  GtkWidget *purpose;
  std::size_t choiceCount = 0;
  std::string label = {};
};

// Rows keep their widget identity while a device is selected or disabled, so
// keyboard focus and an in-progress accessible action survive live previews.
struct OutputDeviceWidgets {
  pipetune::OutputDeviceDescription device;
  std::string outputId = {};
  GtkWidget *row = nullptr;
  GtkWidget *button = nullptr;
  GtkWidget *details = nullptr;
  GtkWidget *presence = nullptr;
  GtkWidget *notice = nullptr;
  GtkWidget *expander = nullptr;
  GtkWidget *contents = nullptr;
  GtkWidget *reassign = nullptr;
  GtkWidget *grid = nullptr;
  GtkWidget *volume = nullptr;
  std::string volumeText = {};
  GtkWidget *timing = nullptr;
  std::string timingText = {};
  std::vector<OutputChannelWidgets> channels = {};
  bool enabled = false;
};

struct OutputMappingReview {
  pipetune::OutputConfiguration before;
  pipetune::OutputConfigurationResult proposal;
  std::string replacementId;
  std::vector<pipetune::AvailableOutput> choices;
  // An available device can take over a saved assignment directly from its row.
  std::optional<pipetune::OutputDeviceDescription> replacementTarget = {};
};

struct ApplicationRunResult {
  int exitCode;
  bool restartRequested;
};

struct GtkRuntime {
  GtkApplication *application;
  ApplicationState state;
  UserSetupClient *userSetupClient;
  bool userSetupPending;
  std::uint64_t userSetupActionId;
  ControlClient *controlClient;
  TrayBackendState *trayBackend;
  TrayBackendAvailabilityState trayAvailability;
  std::filesystem::path startupConfigPath;
  pipetune::StartupConfig savedConfig;
  bool startupConfigAvailable;
  UiLocalizationEnvironment originalLocalization;
  std::filesystem::path uiLanguageConfigPath;
  UiLanguage presentationLanguage;
  UiLanguage savedUiLanguage;
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
  bool savedPresetCatalogParsed;
  std::filesystem::path checkedActiveSavedPresetPath;
  std::string presetCatalogSourceDiagnostic;
  std::string presetCatalogSavedDiagnostic;
  std::vector<SampleRateChoice> rateChoices;
  std::vector<std::string> rateEnforcementChoices;
  std::vector<DspBackendChoice> dspBackendChoices;
  std::uint32_t dspIdleTimeoutSelectionMilliseconds;
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
  bool restartRequested;
  GtkWidget *uiLanguageRestartDialog;
  MainWindowUi ui;
  GdkPixbuf *statusColorIcon;
  GdkPixbuf *statusGrayscaleIcon;
  std::string outputEditError = {};
  std::vector<OutputDeviceWidgets> outputDeviceRows = {};
  std::vector<OutputChannelWidgets> outputReservedChannels = {};
  std::optional<OutputMappingReview> outputMappingReview = {};
};

static void render(GtkRuntime *runtime);
static void driveSettings(GtkRuntime *runtime);
static void beginTransactionFromRuntime(GtkRuntime *runtime);
static void presentWindow(GtkRuntime *runtime,
                          std::optional<guint32> userInteractionTime);
static void requestQuit(GtkRuntime *runtime);
static void scheduleReconnect(GtkRuntime *runtime);

static std::vector<std::string> copyProcessArguments(int argc,
                                                     char **argv) {
  auto arguments = std::vector<std::string>{};
  arguments.reserve(static_cast<std::size_t>(argc));
  for (auto index = int{0}; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return arguments;
}

static std::filesystem::path currentExecutablePath() {
  auto error = std::error_code{};
  const auto executable =
      std::filesystem::read_symlink("/proc/self/exe", error);
  return error ? std::filesystem::path{} : executable;
}

static std::filesystem::path pipeTuneExecutablePath() {
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
  const auto *overridePath =
      std::getenv("PIPETUNE_GTK_E2E_PIPETUNE_EXECUTABLE");
  if (overridePath != nullptr && overridePath[0] != '\0') {
    return overridePath;
  }
#endif
  return kPipeTuneExecutable;
}

static int restartApplicationProcess(
    const std::filesystem::path &executable,
    const std::vector<std::string> &processArguments) {
  if (executable.empty() || processArguments.empty()) {
    std::cerr << "pipetune-gtk: cannot resolve executable for restart\n";
    return 1;
  }
  auto argumentStorage = processArguments;
  auto argumentPointers = std::vector<char *>{};
  argumentPointers.reserve(argumentStorage.size() + 1U);
  for (auto &argument : argumentStorage) {
    argumentPointers.push_back(argument.data());
  }
  argumentPointers.push_back(nullptr);
  ::execv(executable.c_str(), argumentPointers.data());
  const auto error = errno;
  std::cerr << "pipetune-gtk: restart failed: "
            << std::strerror(error) << '\n';
  return 1;
}

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
      .dspIdlePolicy = {},
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

static std::string connectionSummary(const GtkRuntime &runtime) {
  if (runtime.userSetupPending) {
    return translate("Setting up PipeTune for this user…");
  }
  const auto &state = runtime.state;
  switch (state.connection) {
  case ControlConnectionState::connecting:
    return translate("Connecting to the control service…");
  case ControlConnectionState::disconnected:
    return translate("Control service unavailable");
  case ControlConnectionState::connected:
    break;
  }
  if (state.hasRuntimeStatus && state.runtime.rateTransitioning) {
    return translate("Connected · changing sampling frequency");
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
    return localizedMessage("Changing sampling-frequency policy", {});
  case SettingsOperation::dspBackend:
    return localizedMessage("Changing DSP backend", {});
  case SettingsOperation::dspIdle:
    return localizedMessage("Changing silence suspension", {});
  case SettingsOperation::output:
    return localizedMessage("Changing audio outputs", {});
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
    return localizedMessage("Sampling-frequency policy changed", {});
  case SettingsOperation::dspBackend:
    return localizedMessage("DSP backend changed", {});
  case SettingsOperation::dspIdle:
    return localizedMessage("Silence suspension changed", {});
  case SettingsOperation::output:
    return localizedMessage("Audio outputs changed", {});
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
    return localizedMessage("Changing sampling-frequency policy failed", {});
  case SettingsOperation::dspBackend:
    return localizedMessage("Changing DSP backend failed", {});
  case SettingsOperation::dspIdle:
    return localizedMessage("Changing silence suspension failed", {});
  case SettingsOperation::output:
    return localizedMessage("Changing audio outputs failed", {});
  case SettingsOperation::processing:
    return localizedMessage("Changing processing mode failed", {});
  case SettingsOperation::none:
    return localizedMessage("Updating settings failed", {});
  }
  return localizedMessage("Updating settings failed", {});
}

static bool languagePreferenceIsDirty(
    const GtkRuntime &runtime) noexcept {
  return runtime.uiLanguage != runtime.savedUiLanguage;
}

static bool dialogCanApply(const GtkRuntime &runtime) noexcept {
  if (!runtime.dialogActive || runtime.userSetupPending) {
    return false;
  }
  const auto languageDirty = languagePreferenceIsDirty(runtime);
  if (!runtime.transactionReady) {
    return languageDirty;
  }
  const auto settingsApplicable =
      settingsTransactionCanApply(runtime.transaction);
  const auto settingsDirty =
      settingsTransactionIsDirty(runtime.transaction);
  return settingsApplicable || (languageDirty && !settingsDirty);
}

static std::string transactionStateText(const GtkRuntime &runtime) {
  if (!runtime.dialogActive) {
    return {};
  }
  if (runtime.userSetupPending) {
    return translate("Setting up PipeTune for this user…");
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
  if (settingsTransactionIsDirty(transaction) ||
      languagePreferenceIsDirty(runtime)) {
    return translate("Live preview active · changes are not saved");
  }
  return translate("Live settings match the saved configuration");
}

static bool controlsAreEditable(const GtkRuntime &runtime) {
  return runtime.dialogActive && !runtime.userSetupPending &&
         runtime.startupConfigAvailable &&
         runtime.transactionReady &&
         runtime.transaction.connected &&
         !runtime.transaction.cancelRequested &&
         !runtime.transaction.conflict;
}

template <typename Choice>
static bool choiceLabelsMatch(const std::vector<Choice> &left,
                              const std::vector<Choice> &right) {
  if (left.size() != right.size()) {
    return false;
  }
  for (auto index = std::size_t{0}; index < left.size(); ++index) {
    if (left[index].label != right[index].label) {
      return false;
    }
  }
  return true;
}

static void setComboBoxActive(GtkWidget *widget,
                              std::size_t activeIndex) {
  const auto active = static_cast<gint>(activeIndex);
  if (gtk_combo_box_get_active(GTK_COMBO_BOX(widget)) != active) {
    gtk_combo_box_set_active(GTK_COMBO_BOX(widget), active);
  }
}

static std::filesystem::path savedPresetSnapshotDirectory(
    const GtkRuntime &runtime) {
  return runtime.startupConfigPath.parent_path() / "effetune-presets";
}

static void renderPresetControls(GtkRuntime *runtime) {
  const auto &settings = runtime->transactionReady
                             ? runtime->transaction.desiredLive
                             : runtime->savedConfig;
  const auto active = settings.presetFound ? TRUE : FALSE;
  auto *processing = GTK_SWITCH(runtime->ui.processingEnabledSwitch);
  if (gtk_switch_get_active(processing) != active) {
    gtk_switch_set_active(processing, active);
  }
  if (settings.presetFound) {
    runtime->lastPresetPath = settings.presetPath;
  }
  const auto selected = findPresetChoiceIndex(
      runtime->presetChoices, runtime->lastPresetPath,
      savedPresetSnapshotDirectory(*runtime));
  setComboBoxActive(runtime->ui.presetCombo,
                    selected.has_value() ? *selected + 1 : 0);
  renderPresetConfiguration(runtime->ui, runtime->state.presetEntries);
}

static void onOutputDeviceToggled(GtkToggleButton *button, gpointer userData);
static void onOutputChannelChanged(GtkComboBox *combo, gpointer userData);
static void onOutputPurposeActivated(GtkEntry *entry, gpointer userData);
static gboolean onOutputPurposeFocusOut(GtkWidget *entry, GdkEventFocus *, gpointer userData);
static void onOutputReassignClicked(GtkButton *button, gpointer userData);

static void renderOutputMappingActions(GtkRuntime *runtime) {
  if (runtime->outputMappingReview.has_value()) {
    const auto &review = *runtime->outputMappingReview;
    gtk_widget_set_sensitive(runtime->ui.outputMappingUseButton, controlsAreEditable(*runtime) &&
        review.proposal.error.empty() && review.proposal.configuration != review.before);
  }
}

static const char *outputPresence(pipetune::OutputConnectionState state) {
  switch (state) {
  case pipetune::OutputConnectionState::connected: return translate("Connected");
  case pipetune::OutputConnectionState::disabled: return translate("Disabled");
  case pipetune::OutputConnectionState::missing: return translate("Not connected");
  case pipetune::OutputConnectionState::profileMismatch: return translate("Profile changed");
  case pipetune::OutputConnectionState::ambiguous: return translate("Ambiguous device");
  }
  return "";
}

static GtkWidget *outputText(const char *text) {
  auto *label = gtk_label_new(text);
  gtk_label_set_xalign(GTK_LABEL(label), 0);
  gtk_label_set_line_wrap(GTK_LABEL(label), TRUE);
  gtk_label_set_max_width_chars(GTK_LABEL(label), 40);
  return label;
}

// Editors are retained by physical channel, not by DSP slot. Inventory and
// telemetry updates must not replace the entry or overwrite unfinished typing.
static void renderOutputChannels(GtkRuntime *runtime, GtkWidget *grid,
    std::vector<OutputChannelWidgets> &widgets, const std::vector<std::size_t> &slots,
    const std::vector<std::string> &names, const std::string &id) {
  const auto &configuration = runtime->ui.displayedOutputConfiguration.value();
  if (gtk_grid_get_child_at(GTK_GRID(grid), 0, 0) == nullptr) {
    gtk_grid_set_row_spacing(GTK_GRID(grid), 4);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);
    const auto headings = std::array{translate("Device channel"), translate("EffeTune output"), translate("Purpose")};
    for (auto column = std::size_t{0}; column < headings.size(); ++column) {
      gtk_grid_attach(GTK_GRID(grid), outputText(headings[column]), static_cast<int>(column), 0, 1, 1);
    }
  }
  while (widgets.size() > slots.size()) {
    const auto &last = widgets.back();
    gtk_widget_destroy(last.physical);
    gtk_widget_destroy(last.number);
    gtk_widget_destroy(last.purpose);
    widgets.pop_back();
  }
  for (auto index = std::size_t{0}; index < slots.size(); ++index) {
    if (index == widgets.size()) {
      auto channel = OutputChannelWidgets{outputText(""), gtk_combo_box_text_new(), gtk_entry_new()};
      gtk_entry_set_width_chars(GTK_ENTRY(channel.purpose), 8);
      gtk_widget_set_hexpand(channel.purpose, TRUE);
      gtk_widget_set_tooltip_text(channel.purpose, translate("Press Enter or leave the field to preview the purpose label."));
      gtk_grid_attach(GTK_GRID(grid), channel.physical, 0, static_cast<int>(index + 1), 1, 1);
      gtk_grid_attach(GTK_GRID(grid), channel.number, 1, static_cast<int>(index + 1), 1, 1);
      gtk_grid_attach(GTK_GRID(grid), channel.purpose, 2, static_cast<int>(index + 1), 1, 1);
      g_signal_connect(channel.number, "changed", G_CALLBACK(onOutputChannelChanged), runtime);
      g_signal_connect(channel.purpose, "activate", G_CALLBACK(onOutputPurposeActivated), runtime);
      g_signal_connect(channel.purpose, "focus-out-event", G_CALLBACK(onOutputPurposeFocusOut), runtime);
      widgets.push_back(std::move(channel));
    }
    auto &channel = widgets[index];
    const auto assigned = slots[index] < configuration.channels.size();
    const auto count = assigned ? configuration.channels.size() : 0;
    // The first unassigned render still needs its explanatory item.
    if (channel.choiceCount != count || gtk_combo_box_get_active(GTK_COMBO_BOX(channel.number)) < 0) {
      gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(channel.number));
      if (!assigned) gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(channel.number), translate("Unassigned"));
      for (auto slot = std::size_t{0}; slot < count; ++slot) {
        const auto number = "Ch " + std::to_string(slot + 1);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(channel.number), number.c_str());
      }
      channel.choiceCount = count;
    }
    setComboBoxActive(channel.number, assigned ? slots[index] : 0);
    gtk_label_set_text(GTK_LABEL(channel.physical), names[index].c_str());
    gtk_widget_set_sensitive(channel.number, assigned);
    gtk_widget_set_sensitive(channel.purpose, assigned);
    const auto label = assigned ? configuration.channels[slots[index]].label : std::string{};
    if (!assigned || channel.label != label) {
      gtk_entry_set_text(GTK_ENTRY(channel.purpose), label.c_str());
      channel.label = label;
    }
    const auto slotData = GUINT_TO_POINTER(assigned ? static_cast<guint>(slots[index] + 1) : 0);
    g_object_set_data(G_OBJECT(channel.number), "output-slot", slotData);
    g_object_set_data(G_OBJECT(channel.purpose), "output-slot", slotData);
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
    const auto suffix = id + "-" + std::to_string(index);
    gestament_gtk_assign_accessible_id(channel.physical, ("output-physical-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(channel.number, ("output-channel-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(channel.purpose, ("output-purpose-" + suffix).c_str());
#else
    (void)id;
#endif
  }
}

static void renderOutputControls(GtkRuntime *runtime) {
  auto &ui = runtime->ui;
  const auto &configuration = (runtime->transactionReady ? runtime->transaction.desiredLive : runtime->savedConfig).outputConfiguration;
  const auto &status = runtime->state.runtime;
  const auto single = configuration.mode == pipetune::OutputMode::single;
  setComboBoxActive(ui.outputModeCombo, single ? 0 : 1);
  gtk_label_set_text(GTK_LABEL(ui.outputModeNotice), single ?
      translate("The OS selects the output. Saved multiple-output assignments below are inactive.") :
      translate("Input uses Ch 1 and Ch 2. Create additional outputs in your EffeTune preset. Disabled devices keep their Ch numbers."));
  auto diagnostic = runtime->outputEditError;
  if (diagnostic.empty() && !single && std::none_of(configuration.outputs.begin(), configuration.outputs.end(),
      [](const auto &output) { return output.enabled; })) diagnostic = translate("Select at least one output device.");
  if (diagnostic.empty()) diagnostic = pipetune::validateOutputConfiguration(configuration);
  if (diagnostic.empty()) diagnostic = status.outputInventoryError;
  if (diagnostic.empty() && !status.outputInventoryReady) diagnostic = translate("Waiting for output devices…");
  gtk_label_set_text(GTK_LABEL(ui.outputErrorLabel), diagnostic.c_str());
  gtk_widget_set_visible(ui.outputErrorLabel, !diagnostic.empty());
  gtk_widget_set_sensitive(ui.outputModeCombo, controlsAreEditable(*runtime));
  gtk_widget_set_sensitive(ui.outputDeviceList, controlsAreEditable(*runtime) && !single);
  if (ui.displayedOutputConfiguration == configuration && ui.displayedOutputs == status.availableOutputs &&
      ui.displayedOutputInventoryReady == status.outputInventoryReady && ui.displayedOutputInventoryError == status.outputInventoryError) return;
  ui.displayedOutputConfiguration = configuration;
  ui.displayedOutputs = status.availableOutputs;
  ui.displayedOutputInventoryReady = status.outputInventoryReady;
  ui.displayedOutputInventoryError = status.outputInventoryError;
  auto oldRows = std::move(runtime->outputDeviceRows);
  runtime->outputDeviceRows.clear();
  auto rowIndex = std::size_t{0};
  const auto addDevice = [&](const pipetune::OutputDeviceDescription &device, const std::string &key,
                             const std::string &outputId, bool enabled, const char *presence,
                             const char *notice, bool selectable) {
    const auto previous = std::find_if(oldRows.begin(), oldRows.end(), [&](const auto &row) {
      return (!outputId.empty() && row.outputId == outputId) ||
          (row.device.identity == device.identity && row.device.profile == device.profile &&
           row.device.channelPositions == device.channelPositions);
    });
    auto widgets = OutputDeviceWidgets{device};
    auto wasEnabled = false;
    if (previous != oldRows.end()) {
      widgets = std::move(*previous);
      oldRows.erase(previous);
      wasEnabled = widgets.enabled;
      widgets.device = device;
    } else {
      auto *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      gtk_container_set_border_width(GTK_CONTAINER(box), 6);
      widgets.expander = gtk_expander_new(nullptr);
      auto *header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_box_pack_start(GTK_BOX(header), widgets.expander, FALSE, FALSE, 0);
      widgets.button = gtk_check_button_new_with_label(device.name.c_str());
      widgets.reassign = gtk_button_new_with_label(translate("Reassign device…"));
      gtk_box_pack_start(GTK_BOX(header), widgets.button, TRUE, TRUE, 0);
      gtk_box_pack_end(GTK_BOX(header), widgets.reassign, FALSE, FALSE, 0);
      auto *title = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
      gtk_box_pack_start(GTK_BOX(title), header, FALSE, FALSE, 0);
      widgets.details = outputText("");
      widgets.presence = outputText("");
      auto *summary = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
      gtk_box_pack_start(GTK_BOX(summary), widgets.details, TRUE, TRUE, 0);
      gtk_box_pack_end(GTK_BOX(summary), widgets.presence, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(title), summary, FALSE, FALSE, 0);
      widgets.notice = outputText("");
      gtk_box_pack_start(GTK_BOX(title), widgets.notice, FALSE, FALSE, 0);
      gtk_widget_set_no_show_all(widgets.notice, TRUE);
      gtk_box_pack_start(GTK_BOX(box), title, FALSE, FALSE, 0);
      auto *contents = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
      widgets.contents = contents;
      gtk_widget_set_margin_start(contents, 20);
      gtk_widget_set_margin_top(contents, 6);
      widgets.grid = gtk_grid_new();
      widgets.volume = outputText("");
      widgets.timing = outputText("");
      gtk_box_pack_start(GTK_BOX(contents), widgets.grid, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(contents), widgets.volume, FALSE, FALSE, 0);
      gtk_box_pack_start(GTK_BOX(contents), widgets.timing, FALSE, FALSE, 0);
      // GtkExpander's custom label is not an accessible child container. Keep
      // interactive header controls beside its arrow and reveal the body using
      // the documented notify::expanded pattern instead.
      gtk_box_pack_start(GTK_BOX(box), contents, FALSE, FALSE, 0);
      g_signal_connect(widgets.expander, "notify::expanded",
          G_CALLBACK(+[](GObject *object, GParamSpec *, gpointer body) {
            gtk_widget_set_visible(GTK_WIDGET(body), gtk_expander_get_expanded(GTK_EXPANDER(object)));
          }), contents);
      gtk_container_add(GTK_CONTAINER(ui.outputDeviceList), box);
      widgets.row = gtk_widget_get_parent(box);
      g_signal_connect(widgets.button, "toggled", G_CALLBACK(onOutputDeviceToggled), runtime);
      g_signal_connect(widgets.reassign, "clicked", G_CALLBACK(onOutputReassignClicked), runtime);
    }
    const auto newlyAssigned = widgets.outputId.empty() && !outputId.empty();
    widgets.outputId = outputId;
    widgets.enabled = enabled;
    auto *name = GTK_LABEL(gtk_bin_get_child(GTK_BIN(widgets.button)));
    gtk_label_set_text(name, device.name.c_str());
    gtk_label_set_ellipsize(name, PANGO_ELLIPSIZE_END);
    gtk_label_set_max_width_chars(name, 24);
    gtk_toggle_button_set_active(GTK_TOGGLE_BUTTON(widgets.button), enabled);
    gtk_widget_set_sensitive(widgets.button, selectable);
    gtk_widget_set_tooltip_text(widgets.button, device.name.c_str());
    atk_object_set_name(gtk_widget_get_accessible(widgets.expander), device.name.c_str());
    gtk_button_set_label(GTK_BUTTON(widgets.reassign), outputId.empty() ?
        translate("Use existing channels…") : translate("Reassign device…"));
    gtk_widget_set_sensitive(widgets.reassign, !configuration.outputs.empty() &&
        !device.channelPositions.empty() && device.channelPositions.size() <= 16 &&
        status.outputInventoryReady && status.outputInventoryError.empty());
    for (auto *control : {widgets.button, widgets.reassign})
      g_object_set_data_full(G_OBJECT(control), "output-key", g_strdup(key.c_str()), g_free);
    g_object_set_data(G_OBJECT(widgets.row), "output-order", GUINT_TO_POINTER(static_cast<guint>(rowIndex)));
    auto numbers = std::string{};
    auto slots = std::vector<std::size_t>{};
    auto names = std::vector<std::string>{};
    for (auto physical = std::size_t{0}; physical < std::min(device.channelPositions.size(), std::size_t{16}); ++physical) {
      auto slot = configuration.channels.size();
      if (!outputId.empty()) {
        for (auto index = std::size_t{0}; index < configuration.channels.size(); ++index) {
          if (configuration.channels[index].outputId == outputId && configuration.channels[index].deviceChannel == physical) {
            slot = index;
            break;
          }
        }
      }
      slots.push_back(slot);
      names.push_back(std::to_string(physical + 1) + " (" + device.channelPositions[physical] + ')');
      if (slot < configuration.channels.size()) {
        numbers += numbers.empty() ? " · Ch " : ", ";
        numbers += std::to_string(slot + 1);
      }
    }
    renderOutputChannels(runtime, widgets.grid, widgets.channels, slots, names, std::to_string(rowIndex));
    const auto details = device.profile + " · " + std::to_string(device.channelPositions.size()) + " ch" + numbers;
    gtk_label_set_text(GTK_LABEL(widgets.details), details.c_str());
    gtk_label_set_ellipsize(GTK_LABEL(widgets.details), PANGO_ELLIPSIZE_END);
    gtk_widget_set_tooltip_text(widgets.details, details.c_str());
    gtk_label_set_text(GTK_LABEL(widgets.presence), presence);
    gtk_label_set_text(GTK_LABEL(widgets.notice), notice);
    gtk_widget_set_visible(widgets.notice, notice[0] != '\0');
    if (newlyAssigned || (enabled && !wasEnabled))
      gtk_expander_set_expanded(GTK_EXPANDER(widgets.expander), TRUE);
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
    const auto suffix = std::to_string(rowIndex);
    gestament_gtk_assign_accessible_id(widgets.button, ("output-device-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(widgets.expander, ("output-expander-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(widgets.reassign, ("output-reassign-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(widgets.presence, ("output-presence-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(widgets.notice, ("output-notice-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(widgets.volume, ("output-volume-" + suffix).c_str());
    gestament_gtk_assign_accessible_id(widgets.timing, ("output-timing-" + suffix).c_str());
#endif
    ++rowIndex;
    runtime->outputDeviceRows.push_back(std::move(widgets));
  };
  // Check the saved profile even for disabled devices so the repair action is
  // visible before the user tries to enable a stale assignment.
  auto enabledConfiguration = configuration;
  for (auto &output : enabledConfiguration.outputs) output.enabled = true;
  const auto resolved = pipetune::resolveConfiguredOutputs(enabledConfiguration, status.availableOutputs);
  for (auto index = std::size_t{0}; index < configuration.outputs.size(); ++index) {
    const auto &output = configuration.outputs[index];
    const auto *presence = !status.outputInventoryError.empty() ? translate("Inventory unavailable") :
        !status.outputInventoryReady ? translate("Waiting for output devices…") :
        !output.enabled ? translate("Disabled") : outputPresence(resolved[index].state);
    const auto *notice = "";
    if (status.outputInventoryReady && status.outputInventoryError.empty()) {
      if (resolved[index].state == pipetune::OutputConnectionState::profileMismatch)
        notice = translate("Profile changed. Reassign device to update these channels.");
      else if (resolved[index].state == pipetune::OutputConnectionState::ambiguous)
        notice = translate("Ambiguous device. Reassign device to choose the output.");
      else if (resolved[index].state == pipetune::OutputConnectionState::missing)
        notice = translate("Not connected. Reconnect it or reassign this device.");
      else if (!output.enabled)
        notice = translate("Disabled; channel numbers are retained.");
    }
    addDevice(output.device, "saved:" + output.id, output.id, output.enabled, presence, notice, true);
  }
  for (const auto &output : status.availableOutputs) {
    if (std::any_of(configuration.outputs.begin(), configuration.outputs.end(),
        [&output](const auto &saved) { return saved.device.identity == output.device.identity; })) continue;
    const auto count = output.device.channelPositions.size();
    const auto supported = count > 0 && count <= 16;
    const auto room = configuration.channels.size() + count <= 16;
    const auto unique = std::count_if(status.availableOutputs.begin(), status.availableOutputs.end(),
        [&output](const auto &other) { return other.device.identity == output.device.identity &&
            other.device.profile == output.device.profile && other.device.channelPositions == output.device.channelPositions; }) == 1;
    const auto *notice = !supported ? translate("Unsupported channel count") :
        !unique ? translate("The selected device cannot be identified uniquely.") :
        !room ? translate("The 16-channel limit includes disabled and reserved channels. Use existing channels to replace a saved device.") : "";
    addDevice(output.device, "node:" + output.nodeName, {}, false, translate("Available"), notice, supported && room && unique);
  }
  for (const auto &row : oldRows) gtk_widget_destroy(row.row);
  auto reservedSlots = std::vector<std::size_t>{};
  auto reservedNames = std::vector<std::string>{};
  for (auto index = std::size_t{0}; index < configuration.channels.size(); ++index) {
    if (!configuration.channels[index].outputId.empty()) continue;
    reservedSlots.push_back(index);
    reservedNames.emplace_back(translate("Unassigned"));
  }
  renderOutputChannels(runtime, ui.outputReservedGrid, runtime->outputReservedChannels,
      reservedSlots, reservedNames, "reserved");
  auto *reservedRow = gtk_widget_get_parent(ui.outputReservedExpander);
  g_object_set_data(G_OBJECT(reservedRow), "output-order", GUINT_TO_POINTER(G_MAXUINT));
  gtk_list_box_set_sort_func(GTK_LIST_BOX(ui.outputDeviceList),
      [](GtkListBoxRow *left, GtkListBoxRow *right, gpointer) {
        const auto a = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(left), "output-order"));
        const auto b = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(right), "output-order"));
        return a < b ? -1 : a > b ? 1 : 0;
      }, nullptr, nullptr);
  gtk_widget_show_all(ui.outputDeviceList);
  for (const auto &row : runtime->outputDeviceRows)
    gtk_widget_set_visible(row.contents, gtk_expander_get_expanded(GTK_EXPANDER(row.expander)));
  gtk_widget_set_visible(reservedRow, !reservedSlots.empty());
}

static void renderOutputVolumes(GtkRuntime *runtime) {
  const auto &status = runtime->state.runtime;
  const auto ready = runtime->state.connection == ControlConnectionState::connected &&
      status.outputInventoryReady && status.outputInventoryError.empty();
  for (auto &row : runtime->outputDeviceRows) {
    const auto matches = [&row](const auto &output) {
      return output.device.identity == row.device.identity && output.device.profile == row.device.profile &&
          output.device.channelPositions == row.device.channelPositions;
    };
    const pipetune::OutputVolumeState *volume = nullptr;
    if (ready && std::count_if(status.availableOutputs.begin(), status.availableOutputs.end(), matches) == 1) {
      const auto output = std::find_if(status.availableOutputs.begin(), status.availableOutputs.end(), matches);
      const auto report = std::find_if(status.outputVolumes.begin(), status.outputVolumes.end(),
          [&output](const auto &entry) { return entry.nodeSerial == output->nodeSerial; });
      if (report != status.outputVolumes.end()) volume = &*report;
    }
    const auto text = outputVolumeText(volume);
    if (row.volumeText == text) continue;
    row.volumeText = text;
    gtk_label_set_text(GTK_LABEL(row.volume), text.c_str());
    const auto tooltip = text + '\n' + translate(
        "Reported device controls, separate from the OS master volume. Channel gains show the minimum and maximum across the device.");
    gtk_widget_set_tooltip_text(row.volume, tooltip.c_str());
  }
}

static void renderOutputTimings(GtkRuntime *runtime) {
  const auto &status = runtime->state.runtime;
  if (!runtime->ui.displayedOutputConfiguration) return;
  const auto &configuration = *runtime->ui.displayedOutputConfiguration;
  const auto ready = runtime->state.connection == ControlConnectionState::connected &&
      status.outputInventoryReady && status.outputInventoryError.empty() &&
      configuration == status.outputConfiguration;
  const auto resolved = pipetune::resolveConfiguredOutputs(configuration, status.availableOutputs);
  for (auto &row : runtime->outputDeviceRows) {
    const auto output = std::find_if(configuration.outputs.begin(), configuration.outputs.end(),
        [&row](const auto &saved) {
          return saved.device.identity == row.device.identity && saved.device.profile == row.device.profile &&
              saved.device.channelPositions == row.device.channelPositions;
        });
    const auto visible = configuration.mode == pipetune::OutputMode::multiple &&
        output != configuration.outputs.end() && output->enabled;
    gtk_widget_set_visible(row.timing, visible);
    if (!visible) continue;
    const pipetune::OutputTimingState *timing = nullptr;
    const auto &target = resolved[static_cast<std::size_t>(output - configuration.outputs.begin())];
    if (ready && target.inventoryIndex) {
      const auto serial = status.availableOutputs[*target.inventoryIndex].nodeSerial;
      const auto report = std::find_if(status.outputTimings.begin(), status.outputTimings.end(),
          [&output, serial](const auto &entry) { return entry.outputId == output->id && entry.nodeSerial == serial; });
      if (report != status.outputTimings.end()) timing = &*report;
    }
    const auto text = outputTimingText(timing);
    if (row.timingText == text) continue;
    row.timingText = text;
    gtk_label_set_text(GTK_LABEL(row.timing), text.c_str());
    const auto tooltip = text + '\n' + translate(
        "Estimated from reported output latency. This is not a measurement of the compensation buffer or acoustic arrival time.");
    gtk_widget_set_tooltip_text(row.timing, tooltip.c_str());
  }
}

static void renderRateControls(GtkRuntime *runtime) {
  const auto &settings = runtime->transactionReady
                             ? runtime->transaction.desiredLive
                             : runtime->savedConfig;
  const auto presentation =
      makeRateSelectionPresentation(runtime->state, settings.ratePolicy);
  if (!choiceLabelsMatch(runtime->rateChoices, presentation.choices)) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(runtime->ui.rateCombo));
    for (const auto &choice : presentation.choices) {
      gtk_combo_box_text_append_text(
          GTK_COMBO_BOX_TEXT(runtime->ui.rateCombo), choice.label.c_str());
    }
  }
  runtime->rateChoices = presentation.choices;
  setComboBoxActive(runtime->ui.rateCombo,
                    presentation.activeRateIndex);
  const auto enforcementChoices = std::vector<std::string>{
      translate(
          "Suggest — let PipeWire choose the graph sampling frequency"),
      translate("Force — request the fixed graph sampling frequency"),
  };
  if (runtime->rateEnforcementChoices != enforcementChoices) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo));
    for (const auto &label : enforcementChoices) {
      gtk_combo_box_text_append_text(
          GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo),
          label.c_str());
    }
    runtime->rateEnforcementChoices = enforcementChoices;
  }
  setComboBoxActive(runtime->ui.rateEnforcementCombo,
                    presentation.activeEnforcementIndex);
}

static void renderDspControls(GtkRuntime *runtime) {
  const auto &settings = runtime->transactionReady
                             ? runtime->transaction.desiredLive
                             : runtime->savedConfig;
  const auto backend = makeDspBackendSelectionPresentation(
      runtime->state, settings.dspBackend, settings.dspSimdVariant);
  if (!choiceLabelsMatch(runtime->dspBackendChoices,
                         backend.choices)) {
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(runtime->ui.dspBackendCombo));
    for (const auto &choice : backend.choices) {
      gtk_combo_box_text_append_text(
          GTK_COMBO_BOX_TEXT(runtime->ui.dspBackendCombo),
          choice.label.c_str());
    }
  }
  runtime->dspBackendChoices = backend.choices;
  setComboBoxActive(runtime->ui.dspBackendCombo,
                    backend.activeIndex);
  const auto idleEnabled =
      pipetune::dspIdlePolicyIsEnabled(settings.dspIdlePolicy);
  if (idleEnabled) {
    runtime->dspIdleTimeoutSelectionMilliseconds =
        settings.dspIdlePolicy.timeoutMilliseconds;
  }
  auto *idleSwitch = GTK_SWITCH(runtime->ui.dspIdleEnabledSwitch);
  if ((gtk_switch_get_active(idleSwitch) != FALSE) != idleEnabled) {
    gtk_switch_set_active(idleSwitch, idleEnabled ? TRUE : FALSE);
  }
  const auto timeoutSeconds =
      static_cast<double>(runtime->dspIdleTimeoutSelectionMilliseconds) /
      1000.0;
  if (gtk_spin_button_get_value(
          GTK_SPIN_BUTTON(runtime->ui.dspIdleTimeoutSpin)) !=
      timeoutSeconds) {
    gtk_spin_button_set_value(
        GTK_SPIN_BUTTON(runtime->ui.dspIdleTimeoutSpin), timeoutSeconds);
  }
}

static gint uiLanguageComboIndex(UiLanguage language) noexcept {
  constexpr auto languages = std::array{
      UiLanguage::system,     UiLanguage::english,
      UiLanguage::arabic,     UiLanguage::spanish,
      UiLanguage::french,     UiLanguage::hindi,
      UiLanguage::japanese,   UiLanguage::korean,
      UiLanguage::portuguese, UiLanguage::russian,
      UiLanguage::chinese,
  };
  for (auto index = std::size_t{0}; index < languages.size(); ++index) {
    if (languages[index] == language) {
      return static_cast<gint>(index);
    }
  }
  return 0;
}

static bool uiLanguageFromComboIndex(gint index,
                                     UiLanguage *language) noexcept {
  if (language == nullptr) {
    return false;
  }
  constexpr auto languages = std::array{
      UiLanguage::system,     UiLanguage::english,
      UiLanguage::arabic,     UiLanguage::spanish,
      UiLanguage::french,     UiLanguage::hindi,
      UiLanguage::japanese,   UiLanguage::korean,
      UiLanguage::portuguese, UiLanguage::russian,
      UiLanguage::chinese,
  };
  if (index < 0 || static_cast<std::size_t>(index) >= languages.size()) {
    return false;
  }
  *language = languages[static_cast<std::size_t>(index)];
  return true;
}

static void renderLanguageControl(GtkRuntime *runtime) {
  const auto active = uiLanguageComboIndex(runtime->uiLanguage);
  if (gtk_combo_box_get_active(
          GTK_COMBO_BOX(runtime->ui.languageCombo)) != active) {
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(runtime->ui.languageCombo), active);
  }
  if (runtime->languageRestartRequired) {
    gtk_widget_show(runtime->ui.languageRestartNotice);
  } else {
    gtk_widget_hide(runtime->ui.languageRestartNotice);
  }
}

static void renderSettingsControls(GtkRuntime *runtime) {
  runtime->updatingControls = true;
  renderPresetControls(runtime);
  renderOutputControls(runtime);
  renderOutputVolumes(runtime);
  renderOutputTimings(runtime);
  renderOutputMappingActions(runtime);
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
  gtk_widget_set_sensitive(runtime->ui.dspIdleEnabledSwitch, editable);
  const auto &dspSettings = runtime->transactionReady
                                ? runtime->transaction.desiredLive
                                : runtime->savedConfig;
  gtk_widget_set_sensitive(
      runtime->ui.dspIdleTimeoutSpin,
      editable &&
          pipetune::dspIdlePolicyIsEnabled(dspSettings.dspIdlePolicy));
  gtk_widget_set_sensitive(runtime->ui.languageCombo,
                           runtime->dialogActive &&
                               !runtime->userSetupPending);
  gtk_widget_set_sensitive(runtime->ui.restoreDefaultsButton,
                           runtime->dialogActive &&
                               !runtime->userSetupPending);
  gtk_widget_set_sensitive(
      runtime->ui.applyButton,
      dialogCanApply(*runtime));
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
  const auto summary = connectionSummary(*runtime);
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
  // Escape can cancel while an entry still contains text that has never been
  // previewed. Discard it as well as the transaction's committed live edits.
  const auto discardTyping = [](const auto &channels) {
    for (const auto &channel : channels)
      gtk_entry_set_text(GTK_ENTRY(channel.purpose), channel.label.c_str());
  };
  for (const auto &row : runtime->outputDeviceRows) discardTyping(row.channels);
  discardTyping(runtime->outputReservedChannels);
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
  runtime->uiLanguage = runtime->savedUiLanguage;
  runtime->languageRestartRequired =
      runtime->uiLanguage != runtime->presentationLanguage;
  runtime->closeAfterRollback = true;
  runtime->quitAfterRollback =
      quitWhenComplete ||
      runtime->trayAvailability != TrayBackendAvailabilityState::available;
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
  runtime->outputEditError.clear();
  render(runtime);
  driveSettings(runtime);
}

static void onOutputModeChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) return;
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected < 0 || selected > 1) return;
  auto desired = runtime->transaction.desiredLive;
  desired.outputConfiguration.mode = selected == 0 ? pipetune::OutputMode::single : pipetune::OutputMode::multiple;
  editDesiredSettings(runtime, desired);
}

static void onOutputDeviceToggled(GtkToggleButton *button, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) return;
  const auto *stored = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "output-key"));
  if (stored == nullptr) return;
  const auto key = std::string(stored);
  auto desired = runtime->transaction.desiredLive;
  auto &configuration = desired.outputConfiguration;
  if (key.starts_with("saved:")) {
    const auto output = std::find_if(configuration.outputs.begin(), configuration.outputs.end(),
        [&key](const auto &item) { return item.id == key.substr(6); });
    if (output == configuration.outputs.end()) return;
    output->enabled = gtk_toggle_button_get_active(button) != FALSE;
  } else {
    const auto &inventory = runtime->ui.displayedOutputs;
    const auto output = std::find_if(inventory.begin(), inventory.end(),
        [&key](const auto &item) { return item.nodeName == key.substr(5); });
    if (output == inventory.end()) return;
    auto id = std::string{};
    for (auto index = std::size_t{1}; id.empty(); ++index) {
      const auto proposed = "output-" + std::to_string(index);
      if (std::none_of(configuration.outputs.begin(), configuration.outputs.end(),
          [&proposed](const auto &item) { return item.id == proposed; })) id = proposed;
    }
    auto added = pipetune::appendConfiguredOutput(configuration, {id, true, output->device});
    if (added.error.empty()) {
      const auto resolved = pipetune::resolveConfiguredOutputs(added.configuration, inventory);
      if (resolved.back().state != pipetune::OutputConnectionState::connected)
        added.error = translate("The selected device cannot be identified uniquely.");
    }
    if (!added.error.empty()) {
      runtime->outputEditError = std::move(added.error);
      runtime->ui.displayedOutputConfiguration.reset();
      render(runtime);
      return;
    }
    configuration = std::move(added.configuration);
  }
  editDesiredSettings(runtime, desired);
}

static void onOutputPurposeActivated(GtkEntry *entry, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) return;
  const auto slot = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(entry), "output-slot"));
  if (slot == 0) return;
  auto desired = runtime->transaction.desiredLive;
  if (slot > desired.outputConfiguration.channels.size()) return;
  auto &label = desired.outputConfiguration.channels[slot - 1].label;
  const auto *text = gtk_entry_get_text(entry);
  if (label == text) return;
  label = text;
  editDesiredSettings(runtime, desired);
}

static gboolean onOutputPurposeFocusOut(GtkWidget *entry, GdkEventFocus *, gpointer userData) {
  onOutputPurposeActivated(GTK_ENTRY(entry), userData);
  return FALSE;
}

static std::string outputMappingCell(const pipetune::OutputConfiguration &configuration, std::size_t index) {
  if (index >= configuration.channels.size()) return "—";
  const auto &slot = configuration.channels[index];
  const auto output = std::find_if(configuration.outputs.begin(), configuration.outputs.end(),
      [&slot](const auto &item) { return item.id == slot.outputId; });
  auto text = std::string(translate("Reserved"));
  if (output != configuration.outputs.end()) {
    text = output->device.name + " · " + std::to_string(slot.deviceChannel + 1) +
        " (" + output->device.channelPositions[slot.deviceChannel] + ')';
  }
  if (!slot.label.empty()) text += " · " + slot.label;
  return text;
}

static void renderOutputMappingReview(GtkRuntime *runtime) {
  if (!runtime->outputMappingReview.has_value()) return;
  const auto &review = *runtime->outputMappingReview;
  auto *model = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(runtime->ui.outputMappingPreview)));
  gtk_list_store_clear(model);
  const auto count = std::max(review.before.channels.size(), review.proposal.configuration.channels.size());
  for (auto index = std::size_t{0}; index < count; ++index) {
    const auto number = "Ch " + std::to_string(index + 1);
    const auto before = outputMappingCell(review.before, index);
    const auto after = outputMappingCell(review.proposal.configuration, index);
    const auto tooltip = before + " → " + after;
    gtk_list_store_insert_with_values(model, nullptr, -1, 0, number.c_str(),
        1, before.c_str(), 2, after.c_str(), 3, tooltip.c_str(), -1);
  }
  gtk_label_set_text(GTK_LABEL(runtime->ui.outputMappingErrorLabel), review.proposal.error.c_str());
  gtk_widget_set_visible(runtime->ui.outputMappingErrorLabel, !review.proposal.error.empty());
  renderOutputMappingActions(runtime);
}

static void onOutputReplacementChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!runtime->outputMappingReview.has_value()) return;
  auto &review = *runtime->outputMappingReview;
  if (review.replacementId.empty() && !review.replacementTarget) return;
  const auto index = gtk_combo_box_get_active(combo);
  const auto count = review.replacementTarget ? review.before.outputs.size() : review.choices.size();
  if (index < 0 || static_cast<std::size_t>(index) >= count) {
    review.proposal = {review.before, translate("Select a replacement output device.")};
  } else {
    auto candidate = review.before;
    if (review.replacementTarget) {
      review.replacementId = candidate.outputs[index].id;
      // Taking over an assignment also selects the new device. Validate that
      // complete operation even when the draft currently has no enabled output.
      candidate.outputs[index].enabled = true;
    }
    const auto &device = review.replacementTarget ? *review.replacementTarget : review.choices[index].device;
    review.proposal = replaceOutputDevice(candidate, review.replacementId, device);
    if (review.proposal.error.empty()) {
      auto enabled = review.proposal.configuration;
      // A disabled output must still have a unique replacement; temporarily
      // resolve it as enabled without changing the proposed saved choice.
      for (auto &output : enabled.outputs) if (output.id == review.replacementId) output.enabled = true;
      const auto resolved = pipetune::resolveConfiguredOutputs(enabled, review.choices);
      for (const auto &output : resolved) {
        if (output.outputId == review.replacementId && output.state != pipetune::OutputConnectionState::connected)
          review.proposal.error = translate("The selected device cannot be identified uniquely.");
      }
    }
  }
  renderOutputMappingReview(runtime);
}

static void showOutputMappingReview(GtkRuntime *runtime, OutputMappingReview review) {
  runtime->outputMappingReview = std::move(review);
  auto *combo = GTK_COMBO_BOX_TEXT(runtime->ui.outputReplacementCombo);
  gtk_combo_box_text_remove_all(combo);
  const auto &current = *runtime->outputMappingReview;
  const auto append = [combo](const pipetune::OutputDeviceDescription &device) {
    const auto label = device.name + " · " + device.profile + " · " +
        std::to_string(device.channelPositions.size()) + " ch";
    gtk_combo_box_text_append_text(combo, label.c_str());
  };
  if (current.replacementTarget) {
    for (const auto &output : current.before.outputs) append(output.device);
  } else {
    for (const auto &output : current.choices) append(output.device);
  }
  gtk_widget_show_all(runtime->ui.outputMappingDialog);
  gtk_widget_set_visible(runtime->ui.outputReplacementCombo,
      current.replacementTarget.has_value() || !current.replacementId.empty());
  renderOutputMappingReview(runtime);
}

static void onOutputChannelChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) return;
  const auto slot = GPOINTER_TO_UINT(g_object_get_data(G_OBJECT(combo), "output-slot"));
  const auto selected = gtk_combo_box_get_active(combo);
  if (slot == 0 || selected < 0 || static_cast<guint>(selected) == slot - 1) return;
  const auto &configuration = runtime->transaction.desiredLive.outputConfiguration;
  auto proposal = moveOutputChannel(configuration, slot - 1, static_cast<std::size_t>(selected));
  showOutputMappingReview(runtime, {configuration, std::move(proposal), {}, {}});
}

static void onOutputReassignClicked(GtkButton *button, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!controlsAreEditable(*runtime)) return;
  const auto *stored = static_cast<const char *>(g_object_get_data(G_OBJECT(button), "output-key"));
  if (stored == nullptr) return;
  const auto key = std::string(stored);
  const auto &configuration = runtime->transaction.desiredLive.outputConfiguration;
  const auto &inventory = runtime->state.runtime.availableOutputs;
  if (key.starts_with("saved:")) {
    showOutputMappingReview(runtime, {configuration,
        {configuration, translate("Select a replacement output device.")}, key.substr(6), inventory});
  } else {
    const auto found = std::find_if(inventory.begin(), inventory.end(),
        [&key](const auto &output) { return output.nodeName == key.substr(5); });
    if (found == inventory.end()) return;
    showOutputMappingReview(runtime, {configuration,
        {configuration, translate("Select the saved device whose channels this output will use.")}, {}, inventory, found->device});
  }
}

static void onOutputMappingResponse(GtkDialog *, gint response, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!runtime->outputMappingReview.has_value()) return;
  auto &review = *runtime->outputMappingReview;
  if (response != GTK_RESPONSE_ACCEPT) {
    gtk_widget_hide(runtime->ui.outputMappingDialog);
    runtime->outputMappingReview.reset();
    // A number selector displays the proposal while its modal review is open.
    // Cancelling restores the confirmed draft without sending a live change.
    runtime->ui.displayedOutputConfiguration.reset();
    render(runtime);
    return;
  }
  if (!controlsAreEditable(*runtime) || runtime->transaction.desiredLive.outputConfiguration != review.before) {
    review.proposal.error = translate("Output settings changed. Reopen the mapping review.");
  }
  if (review.proposal.error.empty() && !review.replacementId.empty()) {
    auto enabled = review.proposal.configuration;
    for (auto &output : enabled.outputs) if (output.id == review.replacementId) output.enabled = true;
    const auto resolved = pipetune::resolveConfiguredOutputs(enabled, runtime->state.runtime.availableOutputs);
    for (const auto &output : resolved) {
      if (output.outputId == review.replacementId && (output.state != pipetune::OutputConnectionState::connected ||
          !runtime->state.runtime.outputInventoryReady || !runtime->state.runtime.outputInventoryError.empty())) {
        review.proposal.error = translate("The selected output is no longer available with this profile. Reopen the mapping review.");
      }
    }
  }
  if (!review.proposal.error.empty()) {
    renderOutputMappingReview(runtime);
    return;
  }
  auto desired = runtime->transaction.desiredLive;
  desired.outputConfiguration = std::move(review.proposal.configuration);
  gtk_widget_hide(runtime->ui.outputMappingDialog);
  runtime->outputMappingReview.reset();
  editDesiredSettings(runtime, desired);
}

static gboolean onOutputMappingDelete(GtkWidget *, GdkEvent *, gpointer userData) {
  onOutputMappingResponse(nullptr, GTK_RESPONSE_CANCEL, userData);
  return TRUE;
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

static void onDspIdleEnabledChanged(GObject *object, GParamSpec *,
                                    gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  auto timeout = runtime->dspIdleTimeoutSelectionMilliseconds;
  const auto selected = pipetune::DspIdlePolicy{
      .timeoutMilliseconds = timeout,
  };
  if (!pipetune::dspIdlePolicyIsValid(selected) ||
      !pipetune::dspIdlePolicyIsEnabled(selected)) {
    timeout = pipetune::kDspIdleTimeoutDefaultMilliseconds;
    runtime->dspIdleTimeoutSelectionMilliseconds = timeout;
  }
  const auto enabled =
      gtk_switch_get_active(GTK_SWITCH(object)) != FALSE;
  auto desired = runtime->transaction.desiredLive;
  desired.dspIdlePolicy = {
      .timeoutMilliseconds =
          enabled ? timeout
                  : pipetune::kDspIdleTimeoutIgnoredMilliseconds,
  };
  editDesiredSettings(runtime, desired);
}

static void onDspIdleTimeoutChanged(GtkSpinButton *spin,
                                    gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingControls || !controlsAreEditable(*runtime)) {
    return;
  }
  const auto timeout = static_cast<std::uint32_t>(std::lround(
      gtk_spin_button_get_value(spin) * 1000.0));
  const auto policy = pipetune::DspIdlePolicy{
      .timeoutMilliseconds = timeout,
  };
  if (!pipetune::dspIdlePolicyIsValid(policy) ||
      !pipetune::dspIdlePolicyIsEnabled(policy)) {
    return;
  }
  runtime->dspIdleTimeoutSelectionMilliseconds = timeout;
  if (gtk_switch_get_active(
          GTK_SWITCH(runtime->ui.dspIdleEnabledSwitch)) == FALSE) {
    return;
  }
  auto desired = runtime->transaction.desiredLive;
  desired.dspIdlePolicy = policy;
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
  runtime->uiLanguage = selected;
  runtime->languageRestartRequired =
      runtime->uiLanguage != runtime->presentationLanguage;
  render(runtime);
}

static void onUiLanguageRestartDialogDestroy(GtkWidget *dialog,
                                             gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->uiLanguageRestartDialog == dialog) {
    runtime->uiLanguageRestartDialog = nullptr;
  }
}

static void onUiLanguageRestartDialogResponse(GtkDialog *dialog,
                                              gint response,
                                              gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (response == GTK_RESPONSE_ACCEPT) {
    runtime->restartRequested = true;
    requestQuit(runtime);
  }
}

static void showUiLanguageRestartDialog(GtkRuntime *runtime) {
  if (runtime->uiLanguageRestartDialog != nullptr) {
    gtk_window_present(GTK_WINDOW(runtime->uiLanguageRestartDialog));
    return;
  }
  auto *dialog = gtk_message_dialog_new(
      GTK_WINDOW(runtime->ui.window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_INFO, GTK_BUTTONS_NONE, "%s",
      translate("Restart to apply UI language?"));
  gtk_window_set_title(
      GTK_WINDOW(dialog), translate("Restart to apply UI language?"));
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      translate("The UI language change will take effect after PipeTune "
                "GTK restarts."));
  [[maybe_unused]] auto *later = gtk_dialog_add_button(
      GTK_DIALOG(dialog), translate("Later"), GTK_RESPONSE_CANCEL);
  [[maybe_unused]] auto *restart = gtk_dialog_add_button(
      GTK_DIALOG(dialog), translate("Restart now"), GTK_RESPONSE_ACCEPT);
#ifdef PIPETUNE_GTK_E2E_ACCESSIBILITY
  gestament_gtk_assign_accessible_id(
      dialog, "ui_language_restart_dialog");
  gestament_gtk_assign_accessible_id(
      later, "ui_language_restart_later_button");
  gestament_gtk_assign_accessible_id(
      restart, "ui_language_restart_now_button");
#endif
  gtk_dialog_set_default_response(GTK_DIALOG(dialog), GTK_RESPONSE_CANCEL);
  runtime->uiLanguageRestartDialog = dialog;
  g_signal_connect(dialog, "response",
                   G_CALLBACK(onUiLanguageRestartDialogResponse), runtime);
  g_signal_connect(dialog, "destroy",
                   G_CALLBACK(onUiLanguageRestartDialogDestroy), runtime);
  gtk_widget_show_all(dialog);
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
  for (const auto &choice : runtime->presetChoices) {
    const auto label = presetChoiceLabel(choice);
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo), label.c_str());
  }
  renderPresetControls(runtime);
  runtime->updatingControls = false;
}

static void refreshRuntimeActiveSavedPresetSnapshot(
    GtkRuntime *runtime) {
  if (!runtime->savedPresetCatalogParsed ||
      !runtime->state.hasRuntimeStatus ||
      runtime->state.runtime.processingMode !=
          pipetune::ProcessingMode::preset) {
    return;
  }
  const auto refreshed = refreshActiveSavedPresetSnapshot(
      runtime->presetChoices, runtime->state.runtime.activePreset,
      savedPresetSnapshotDirectory(*runtime));
  if (!refreshed.error.empty()) {
    appendCompletedAction(
        runtime, ActionLogSeverity::error,
        ActionLogCategory::application, false,
        localizedMessage("Cannot prepare preset", {}),
        technicalMessage(refreshed.error));
  }
}

static void refreshSavedPresetCatalog(GtkRuntime *runtime) {
  if (runtime->effetuneUserPresetPath.empty()) {
    return;
  }
  const auto refresh =
      loadEffeTuneSavedPresets(runtime->effetuneUserPresetPath);
  runtime->savedPresetCatalogParsed = refresh.parsed;
  auto choices = applyEffeTuneSavedPresetRefresh(
      runtime->presetChoices, refresh);
  runtime->presetCatalogSavedDiagnostic =
      catalogDiagnosticText(refresh.diagnostics, {});
  replacePresetChoices(runtime, std::move(choices));
  if (refresh.parsed) {
    refreshRuntimeActiveSavedPresetSnapshot(runtime);
  }
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
      choice, savedPresetSnapshotDirectory(*runtime));
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
  selectPresetPath(runtime, path);
}

static void onRestoreDefaultsClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime == nullptr || !runtime->dialogActive) {
    return;
  }
  const auto defaults = defaultStartupConfig();
  runtime->outputEditError.clear();
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
  if (!dialogCanApply(*runtime)) {
    return;
  }
  const auto persistSettings =
      runtime->transactionReady &&
      settingsTransactionCanApply(runtime->transaction);
  const auto persistLanguage = languagePreferenceIsDirty(*runtime);
  auto persistenceTargets = std::string{};
  if (persistSettings) {
    persistenceTargets = runtime->startupConfigPath.string();
  }
  if (persistLanguage) {
    appendDetail(persistenceTargets,
                 runtime->uiLanguageConfigPath.string());
  }
  const auto pending = appendPendingAction(
      runtime->actionLog, currentUnixMilliseconds(),
      ActionLogCategory::persistence,
      localizedMessage("Saving all settings", {}),
      technicalMessage(persistenceTargets));
  auto error = std::string{};
  auto promptForLanguageRestart = false;
  if (persistSettings) {
    error = persistenceTestDiagnostic();
    if (error.empty()) {
      error = pipetune::saveStartupConfig(
          runtime->startupConfigPath, runtime->transaction.desiredLive);
    }
    completeSettingsPersistence(runtime->transaction, error.empty(), error);
    if (error.empty()) {
      runtime->savedConfig = runtime->transaction.saved;
    }
  }
  if (error.empty() && persistLanguage) {
    const auto saved = saveUiLanguagePreference(
        runtime->uiLanguageConfigPath, runtime->uiLanguage);
    error = saved.error;
    if (error.empty()) {
      runtime->savedUiLanguage = runtime->uiLanguage;
      promptForLanguageRestart =
          runtime->savedUiLanguage != runtime->presentationLanguage;
    }
  }
  const auto success = error.empty();
  if (success) {
    completePendingAction(
        runtime->actionLog, pending, currentUnixMilliseconds(), true,
        ActionLogSeverity::info,
        localizedMessage("All settings saved", {}),
        technicalMessage(persistenceTargets));
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
  if (success && promptForLanguageRestart) {
    showUiLanguageRestartDialog(runtime);
  }
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
  case SettingsOperation::dspIdle:
    setControlDspIdleAsync(runtime->controlClient, target.dspIdlePolicy,
                           onSettingsOperationReply, runtime);
    return;
  case SettingsOperation::output: {
    const auto &confirmed = runtime->transaction.confirmedLive;
    auto preset = std::optional<std::filesystem::path>{};
    if (target.presetFound != confirmed.presetFound || target.presetPath != confirmed.presetPath)
      preset = target.presetFound ? target.presetPath : std::filesystem::path{};
    setControlOutputAsync(runtime->controlClient, target.outputConfiguration, preset,
        runtime->transaction.confirmedRevision, onSettingsOperationReply, runtime);
    return;
  }
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
  runtime->outputEditError.clear();
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
  const auto activePresetPath =
      runtime->state.hasRuntimeStatus &&
              runtime->state.runtime.processingMode ==
                  pipetune::ProcessingMode::preset
          ? std::filesystem::path(runtime->state.runtime.activePreset)
          : std::filesystem::path{};
  if (activePresetPath != runtime->checkedActiveSavedPresetPath) {
    runtime->checkedActiveSavedPresetPath = activePresetPath;
    refreshRuntimeActiveSavedPresetSnapshot(runtime);
  }
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
  g_signal_connect(runtime->ui.outputModeCombo, "changed", G_CALLBACK(onOutputModeChanged), runtime);
  g_signal_connect(runtime->ui.outputReplacementCombo, "changed", G_CALLBACK(onOutputReplacementChanged), runtime);
  g_signal_connect(runtime->ui.outputMappingDialog, "response", G_CALLBACK(onOutputMappingResponse), runtime);
  g_signal_connect(runtime->ui.outputMappingDialog, "delete-event", G_CALLBACK(onOutputMappingDelete), runtime);
  g_signal_connect(runtime->ui.rateEnforcementCombo, "changed",
                   G_CALLBACK(onRateEnforcementChanged), runtime);
  g_signal_connect(runtime->ui.dspBackendCombo, "changed",
                   G_CALLBACK(onDspBackendChanged), runtime);
  g_signal_connect(runtime->ui.dspIdleEnabledSwitch, "notify::active",
                   G_CALLBACK(onDspIdleEnabledChanged), runtime);
  g_signal_connect(runtime->ui.dspIdleTimeoutSpin, "value-changed",
                   G_CALLBACK(onDspIdleTimeoutChanged), runtime);
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
                          std::optional<guint32> userInteractionTime) {
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
  if (runtime->shuttingDown) {
    return;
  }
  const auto previous = runtime->trayAvailability;
  runtime->trayAvailability = availability;
  if (previous == TrayBackendAvailabilityState::available &&
      availability == TrayBackendAvailabilityState::unavailable) {
    if (runtime->closeAfterRollback) {
      runtime->quitAfterRollback = true;
    } else if (!runtime->dialogActive) {
      presentWindow(runtime, std::nullopt);
    }
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

static void sendUserSetupFailureNotification(
    GtkRuntime *runtime, std::string_view diagnostic) {
  auto *notification =
      g_notification_new(translate("PipeTune setup failed"));
  const auto body = diagnostic.empty()
                        ? std::string(translate("PipeTune setup failed"))
                        : std::string(diagnostic);
  g_notification_set_body(notification, body.c_str());
  g_notification_set_priority(notification,
                              G_NOTIFICATION_PRIORITY_HIGH);
  g_application_send_notification(
      G_APPLICATION(runtime->application), "user-setup-failed",
      notification);
  g_object_unref(notification);
}

static void onUserSetupCompleted(const UserSetupClientResult &result,
                                 void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->shuttingDown) {
    return;
  }
  if (runtime->userSetupPending) {
    runtime->userSetupPending = false;
    g_application_unmark_busy(G_APPLICATION(runtime->application));
  }

  const auto detail = result.success ? result.standardOutput : result.error;
  completePendingAction(
      runtime->actionLog, runtime->userSetupActionId,
      currentUnixMilliseconds(), result.success,
      result.success ? ActionLogSeverity::info : ActionLogSeverity::error,
      result.success
          ? localizedMessage("PipeTune setup completed", {})
          : localizedMessage("PipeTune setup failed", {}),
      technicalMessage(detail));
  runtime->userSetupActionId = 0;
  if (result.success) {
    g_application_withdraw_notification(
        G_APPLICATION(runtime->application), "user-setup-failed");
  } else {
    revealActionLog(runtime);
    if (runtime->ui.window == nullptr ||
        gtk_widget_get_visible(runtime->ui.window) == FALSE) {
      sendUserSetupFailureNotification(runtime, result.error);
    }
  }
  initializeControlClient(runtime);
  render(runtime);
}

static void initializeUserSetup(GtkRuntime *runtime) {
  const auto executable = pipeTuneExecutablePath();
  runtime->userSetupPending = true;
  runtime->userSetupActionId = appendPendingAction(
      runtime->actionLog, currentUnixMilliseconds(),
      ActionLogCategory::application,
      localizedMessage("Setting up PipeTune for this user…", {}),
      technicalMessage(executable.string()));
  g_application_mark_busy(G_APPLICATION(runtime->application));
  runtime->userSetupClient = createUserSetupClient(executable);
  if (runtime->userSetupClient == nullptr) {
    onUserSetupCompleted(
        {.success = false,
         .standardOutput = {},
         .error = "installed PipeTune executable path is invalid"},
        runtime);
    return;
  }
  if (!setupUserIfNeededAsync(runtime->userSetupClient,
                              onUserSetupCompleted, runtime)) {
    onUserSetupCompleted(
        {.success = false,
         .standardOutput = {},
         .error = "cannot schedule PipeTune user setup"},
        runtime);
  }
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
                  [runtime](const TrayActivationContext &context) {
                    presentWindow(runtime,
                                  context.userInteractionTime);
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
  initializeUserSetup(runtime);
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
      presentWindow(runtime, std::nullopt);
    }
    return 0;
  }
  if (!parsed.options.hidden) {
    presentWindow(runtime, std::nullopt);
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
  if (runtime->userSetupPending) {
    runtime->userSetupPending = false;
    g_application_unmark_busy(G_APPLICATION(runtime->application));
  }
  destroyUserSetupClient(runtime->userSetupClient);
  runtime->userSetupClient = nullptr;
  destroyEffeTunePresetFileMonitor(runtime->presetFileMonitor);
  runtime->presetFileMonitor = nullptr;
  destroyControlClient(runtime->controlClient);
  runtime->controlClient = nullptr;
  destroyTrayBackend(runtime->trayBackend);
  runtime->trayBackend = nullptr;
  if (runtime->uiLanguageRestartDialog != nullptr) {
    gtk_widget_destroy(runtime->uiLanguageRestartDialog);
  }
  destroyMainWindowPresentation(runtime);
  releaseStatusArtwork(runtime);
  releaseApplicationHold(runtime);
}

static ApplicationRunResult runApplication(int argc, char **argv) {
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
  const auto defaultConfig = defaultStartupConfig();
  auto runtime = GtkRuntime{
      .application = application,
      .state = initialApplicationState(),
      .userSetupClient = nullptr,
      .userSetupPending = false,
      .userSetupActionId = 0,
      .controlClient = nullptr,
      .trayBackend = nullptr,
      .trayAvailability = TrayBackendAvailabilityState::pending,
      .startupConfigPath = {},
      .savedConfig = defaultConfig,
      .startupConfigAvailable = false,
      .originalLocalization = originalLocalization,
      .uiLanguageConfigPath = uiLanguageConfigPath,
      .presentationLanguage = presentationLanguage,
      .savedUiLanguage = presentationLanguage,
      .uiLanguage = presentationLanguage,
      .languageRestartRequired = false,
      .uiLanguageLoadWarning = loadedLanguage.warning,
      .localizationWarning = localizationWarning,
      .transaction = beginSettingsTransaction(
          defaultConfig, defaultConfig, 0, false),
      .transactionReady = false,
      .dialogActive = false,
      .updatingControls = false,
      .closeAfterRollback = false,
      .quitAfterRollback = false,
      .lastPresetPath = {},
      .presetChoices = {},
      .effetuneUserPresetPath = {},
      .presetFileMonitor = nullptr,
      .savedPresetCatalogParsed = false,
      .checkedActiveSavedPresetPath = {},
      .presetCatalogSourceDiagnostic = {},
      .presetCatalogSavedDiagnostic = {},
      .rateChoices = {},
      .rateEnforcementChoices = {},
      .dspBackendChoices = {},
      .dspIdleTimeoutSelectionMilliseconds =
          pipetune::kDspIdleTimeoutDefaultMilliseconds,
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
      .restartRequested = false,
      .uiLanguageRestartDialog = nullptr,
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
  const auto restartRequested = runtime.restartRequested;
  g_object_unref(application);
  restoreUiLocalizationEnvironment(originalLocalization);
  return {
      .exitCode = result,
      .restartRequested = restartRequested,
  };
}

} // namespace pipetune_gtk

int main(int argc, char **argv) {
  const auto processArguments =
      pipetune_gtk::copyProcessArguments(argc, argv);
  const auto executable = pipetune_gtk::currentExecutablePath();
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
  const auto result = pipetune_gtk::runApplication(argc, argv);
  if (result.restartRequested) {
    return pipetune_gtk::restartApplicationProcess(
        executable, processArguments);
  }
  return result.exitCode;
}
