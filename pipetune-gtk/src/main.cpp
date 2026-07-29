#include "application-state.h"
#include "configuration-reset-client.h"
#include "control-client.h"
#include "installed-presets.h"
#include "installed-tools.h"
#include "launch-options.h"
#include "main-window.h"
#include "output-operation.h"
#include "output-selection-model.h"
#include "preset-catalog.h"
#include "preset-file-monitor.h"
#include "rate-operation.h"
#include "rate-selection-model.h"
#include "status-icon.h"
#include "status-text.h"
#include "tray-backend.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"
#include "pipetune/version.h"

#include <gtk/gtk.h>

#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pipetune_gtk {

constexpr auto kReconnectDelaySeconds = guint{2};
constexpr auto kStatusArtworkSize = int{48};

struct GtkRuntime {
  GtkApplication *application;
  ApplicationState state;
  ControlClient *controlClient;
  ConfigurationResetClient *configurationResetClient;
  bool configurationResetPending;
  TrayBackendState *trayBackend;
  TrayBackendAvailabilityState trayAvailability;
  std::filesystem::path startupConfigPath;
  std::filesystem::path startupPreset;
  bool hasStartupPreset;
  std::filesystem::path pendingPreset;
  std::vector<PresetChoice> presetChoices;
  std::filesystem::path effetuneUserPresetPath;
  EffeTunePresetFileMonitor *presetFileMonitor;
  bool updatingPresetCombo;
  std::string presetCatalogSourceDiagnostic;
  std::string presetCatalogSavedDiagnostic;
  std::string presetCatalogDiagnostic;
  std::vector<OutputDeviceChoice> outputChoices;
  bool updatingOutputCombo;
  bool outputChangePending;
  bool pendingOutputClear;
  std::string pendingOutputTarget;
  pipetune::SampleRatePolicy startupRatePolicy;
  pipetune::SampleRatePolicy editedRatePolicy;
  pipetune::SampleRatePolicy pendingRatePolicy;
  std::vector<SampleRateChoice> rateChoices;
  bool updatingRateControls;
  bool rateEditDirty;
  bool rateChangePending;
  guint reconnectSource;
  bool applicationHeld;
  bool activationHandled;
  bool shuttingDown;
  bool quitting;
  MainWindowUi ui;
  GdkPixbuf *statusColorIcon;
  GdkPixbuf *statusGrayscaleIcon;
};

static std::string pathText(const std::filesystem::path &path) {
  return path.empty() ? std::string("—") : path.string();
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

static std::string connectionText(const ApplicationState &state) {
  if (state.connection == ControlConnectionState::connecting) {
    return "Connecting to PipeTune…";
  }
  if (state.connection == ControlConnectionState::disconnected) {
    return "PipeTune is disconnected";
  }
  if (state.hasRuntimeStatus && state.runtime.rateTransitioning) {
    return "Connected — switching PCM rate…";
  }
  if (!state.runtime.defaultSinkActive) {
    return "Connected — default sink is inactive";
  }
  return "Connected";
}

static std::string trayTooltip(const ApplicationState &state) {
  if (state.connection == ControlConnectionState::connecting) {
    return "PipeTune: connecting";
  }
  if (state.connection == ControlConnectionState::disconnected) {
    return "PipeTune: disconnected";
  }
  if (trayVisualState(state) == TrayVisualState::attention) {
    return "PipeTune: attention required";
  }
  const auto filename =
      std::filesystem::path(state.runtime.activePreset).filename().string();
  return filename.empty() ? std::string("PipeTune: active")
                          : "PipeTune: " + filename;
}

static void appendNotice(std::string &notice, std::string_view addition) {
  if (addition.empty()) {
    return;
  }
  if (!notice.empty()) {
    notice.push_back('\n');
  }
  notice.append(addition);
}

static std::string noticeText(
    const ApplicationState &state,
    std::string_view presetCatalogDiagnostic) {
  auto notice = state.diagnostic;
  appendNotice(notice, presetCatalogDiagnostic);
  if (state.hasRuntimeStatus &&
      !state.runtime.configurationError.empty()) {
    appendNotice(notice, "Startup configuration: " +
                             state.runtime.configurationError);
  }
  if (state.hasRuntimeStatus && !state.runtime.rateError.empty()) {
    appendNotice(notice, "PCM rate: " + state.runtime.rateError);
  }
  for (const auto &warning : state.warnings) {
    appendNotice(
        notice,
        "Preset node " + std::to_string(warning.nodeIndex + 1) +
            " (\"" + warning.pluginName + "\") was skipped: " +
            warning.reason);
  }
  return notice;
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

static void renderOutputSelection(GtkRuntime *runtime) {
  const auto presentation =
      makeOutputSelectionPresentation(runtime->state);
  gtk_label_set_text(GTK_LABEL(runtime->ui.targetLabel),
                     presentation.effectiveOutput.c_str());
  gtk_label_set_text(GTK_LABEL(runtime->ui.outputReasonLabel),
                     presentation.reason.c_str());

  if (!runtime->outputChangePending) {
    runtime->updatingOutputCombo = true;
    gtk_combo_box_text_remove_all(
        GTK_COMBO_BOX_TEXT(runtime->ui.outputCombo));
    runtime->outputChoices = presentation.choices;
    for (const auto &choice : runtime->outputChoices) {
      gtk_combo_box_text_append_text(
          GTK_COMBO_BOX_TEXT(runtime->ui.outputCombo),
          choice.label.c_str());
    }
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(runtime->ui.outputCombo),
        static_cast<gint>(presentation.activeIndex));
    runtime->updatingOutputCombo = false;
  }
  gtk_widget_set_sensitive(runtime->ui.outputCombo,
                           presentation.sensitive &&
                               !runtime->outputChangePending);
}

static pipetune::SampleRatePolicy displayedRatePolicy(
    const GtkRuntime &runtime) {
  if (runtime.state.connection == ControlConnectionState::connected &&
      runtime.state.hasRuntimeStatus) {
    return runtime.state.runtime.configuredRatePolicy;
  }
  return runtime.startupRatePolicy;
}

static void renderRateSelection(GtkRuntime *runtime) {
  if (!runtime->rateEditDirty && !runtime->rateChangePending) {
    runtime->editedRatePolicy = displayedRatePolicy(*runtime);
  }
  const auto presentation = makeRateSelectionPresentation(
      runtime->state, runtime->editedRatePolicy);

  runtime->updatingRateControls = true;
  runtime->rateChoices = presentation.choices;
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateCombo));
  for (const auto &choice : runtime->rateChoices) {
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(runtime->ui.rateCombo),
        choice.label.c_str());
  }
  gtk_combo_box_set_active(
      GTK_COMBO_BOX(runtime->ui.rateCombo),
      static_cast<gint>(presentation.activeRateIndex));

  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo));
  gtk_combo_box_text_append_text(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo),
      "Suggest — let PipeWire choose");
  gtk_combo_box_text_append_text(
      GTK_COMBO_BOX_TEXT(runtime->ui.rateEnforcementCombo),
      "Force — request the selected output rate");
  gtk_combo_box_set_active(
      GTK_COMBO_BOX(runtime->ui.rateEnforcementCombo),
      static_cast<gint>(presentation.activeEnforcementIndex));
  runtime->updatingRateControls = false;

  gtk_label_set_text(GTK_LABEL(runtime->ui.rateStatusLabel),
                     presentation.effectiveRates.c_str());
  const auto connected =
      runtime->state.connection == ControlConnectionState::connected;
  const auto canEdit =
      !runtime->startupConfigPath.empty() &&
      !runtime->state.operationPending &&
      !runtime->rateChangePending &&
      (!connected || presentation.sensitive);
  gtk_widget_set_sensitive(runtime->ui.rateCombo, canEdit);
  gtk_widget_set_sensitive(runtime->ui.rateEnforcementCombo, canEdit);
  gtk_button_set_label(
      GTK_BUTTON(runtime->ui.rateApplyButton),
      connected ? "Apply and Save" : "Save for Next Start");
  gtk_widget_set_sensitive(runtime->ui.rateApplyButton,
                           canEdit && runtime->rateEditDirty);
}

static void render(GtkRuntime *runtime) {
  if (runtime == nullptr || runtime->ui.window == nullptr) {
    return;
  }
  const auto status = connectionText(runtime->state);
  gtk_label_set_text(GTK_LABEL(runtime->ui.statusLabel), status.c_str());
  const auto iconPresentation =
      statusIconPresentation(runtime->state);
  auto *statusIcon =
      iconPresentation.colorMode == TrayIconColorMode::color
          ? runtime->statusColorIcon
          : runtime->statusGrayscaleIcon;
  gtk_image_set_from_pixbuf(GTK_IMAGE(runtime->ui.statusImage),
                            statusIcon);
  const auto *badge = badgeIconName(iconPresentation.badge);
  if (badge == nullptr) {
    gtk_widget_hide(runtime->ui.statusBadge);
  } else {
    gtk_image_set_from_icon_name(GTK_IMAGE(runtime->ui.statusBadge), badge,
                                 GTK_ICON_SIZE_MENU);
    gtk_widget_show(runtime->ui.statusBadge);
  }

  const auto processingMode =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.processingMode ==
                     pipetune::ProcessingMode::bypass
                 ? "Bypass"
                 : "Preset")
          : "—";
  gtk_label_set_text(GTK_LABEL(runtime->ui.processingModeLabel),
                     processingMode);
  const auto activePreset =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.processingMode ==
                     pipetune::ProcessingMode::bypass
                 ? std::string("None — pass-through")
                 : pathText(runtime->state.runtime.activePreset))
          : std::string("—");
  gtk_label_set_text(GTK_LABEL(runtime->ui.activePresetLabel),
                     activePreset.c_str());
  const auto startupPreset =
      runtime->hasStartupPreset ? pathText(runtime->startupPreset)
                                : std::string("Bypass");
  gtk_label_set_text(GTK_LABEL(runtime->ui.startupPresetLabel),
                     startupPreset.c_str());
  const auto pluginCount =
      runtime->state.hasRuntimeStatus
          ? std::to_string(runtime->state.runtime.activePluginCount)
          : std::string("—");
  gtk_label_set_text(GTK_LABEL(runtime->ui.pluginCountLabel),
                     pluginCount.c_str());
  renderOutputSelection(runtime);
  renderRateSelection(runtime);
  const auto defaultSink =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.defaultSinkActive ? "Active"
                                                      : "Inactive")
          : "—";
  gtk_label_set_text(GTK_LABEL(runtime->ui.defaultSinkLabel),
                     defaultSink);
  const auto inputText =
      inputStatusText(runtime->state, currentUnixMilliseconds());
  gtk_label_set_text(GTK_LABEL(runtime->ui.inputFrameRateLabel),
                     inputText.frameRate.c_str());
  gtk_label_set_text(GTK_LABEL(runtime->ui.lastInputLabel),
                     inputText.lastReceived.c_str());
  gtk_label_set_text(GTK_LABEL(runtime->ui.pcmDataRateLabel),
                     inputText.pcmDataRate.c_str());
  gtk_label_set_text(GTK_LABEL(runtime->ui.streamFormatLabel),
                     inputText.streamFormat.c_str());
  const auto runtimeText = runtimeStatusText(runtime->state);
  gtk_label_set_text(
      GTK_LABEL(runtime->ui.dspProcessingTimeLabel),
      runtimeText.dspProcessingTime.c_str());
  gtk_label_set_text(GTK_LABEL(runtime->ui.counterLabel),
                     runtimeText.counters.c_str());

  const auto notice =
      noticeText(runtime->state, runtime->presetCatalogDiagnostic);
  gtk_label_set_text(GTK_LABEL(runtime->ui.noticeLabel), notice.c_str());
  gtk_widget_set_visible(runtime->ui.noticeBox, !notice.empty());
  gtk_button_set_label(
      GTK_BUTTON(runtime->ui.applyButton),
      runtime->state.connection == ControlConnectionState::connected
          ? "Apply and Save"
          : "Save for Next Start");
  gtk_widget_set_sensitive(runtime->ui.applyButton,
                           !runtime->state.operationPending);
  gtk_button_set_label(
      GTK_BUTTON(runtime->ui.bypassButton),
      runtime->state.connection == ControlConnectionState::connected
          ? "Bypass and Save"
          : "Save Bypass");
  gtk_widget_set_sensitive(runtime->ui.bypassButton,
                           !runtime->state.operationPending);
  gtk_widget_set_sensitive(
      runtime->ui.resetButton,
      runtime->configurationResetClient != nullptr &&
          !runtime->configurationResetPending &&
          !runtime->state.operationPending);
  updateTrayBackend(runtime->trayBackend,
                    iconStateForApplication(runtime->state),
                    iconPresentation.colorMode,
                    trayTooltip(runtime->state));
}

static void presentWindow(GtkRuntime *runtime,
                          guint32 userInteractionTime);
static void requestQuit(GtkRuntime *runtime);
static std::string reloadStartupConfig(GtkRuntime *runtime,
                                       bool resetSelections);

static gboolean onWindowDelete(GtkWidget *, GdkEvent *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!runtime->quitting &&
      runtime->trayAvailability ==
          TrayBackendAvailabilityState::available) {
    gtk_widget_hide(runtime->ui.window);
    return TRUE;
  }
  requestQuit(runtime);
  return TRUE;
}

static void onWindowDestroy(GtkWidget *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  runtime->ui.window = nullptr;
}

static void onNoticeDismiss(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  clearControlNotice(runtime->state);
  runtime->presetCatalogSourceDiagnostic.clear();
  runtime->presetCatalogSavedDiagnostic.clear();
  runtime->presetCatalogDiagnostic.clear();
  render(runtime);
}

static void scheduleReconnect(GtkRuntime *runtime);

static void onSubscriptionMessage(
    const pipetune::ControlResponseParseResult &message, void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  applyControlResponse(runtime->state, message,
                       currentMonotonicMilliseconds());
  render(runtime);
}

static void onConnectionChanged(bool connected, std::string_view error,
                                void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->shuttingDown) {
    return;
  }
  if (connected) {
    return;
  }
  markControlDisconnected(runtime->state, error);
  if (runtime->configurationResetPending) {
    setControlOperationPending(runtime->state, true);
  }
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

static void reconnectControlImmediately(GtkRuntime *runtime) {
  if (runtime->controlClient == nullptr || runtime->shuttingDown) {
    return;
  }
  if (runtime->reconnectSource != 0) {
    g_source_remove(runtime->reconnectSource);
    runtime->reconnectSource = 0;
  }
  stopControlSubscription(runtime->controlClient);
  markControlConnecting(runtime->state);
  startControlSubscription(runtime->controlClient);
}

static void onConfigurationResetCompleted(
    const ConfigurationResetClientResult &result, void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->shuttingDown) {
    return;
  }
  runtime->configurationResetPending = false;
  setControlOperationPending(runtime->state, false);

  const auto reloadError = reloadStartupConfig(runtime, true);
  reconnectControlImmediately(runtime);

  auto diagnostic =
      result.success ? result.standardOutput : result.error;
  if (diagnostic.empty()) {
    diagnostic =
        result.success
            ? std::string{"PipeTune configuration was reset"}
            : std::string{"PipeTune configuration reset failed"};
  }
  if (!reloadError.empty()) {
    appendNotice(diagnostic,
                 "Configuration reload failed: " + reloadError);
  }
  setControlDiagnostic(runtime->state, diagnostic);
  render(runtime);
}

static void startConfigurationReset(GtkRuntime *runtime) {
  if (runtime->configurationResetClient == nullptr ||
      runtime->configurationResetPending ||
      runtime->state.operationPending || runtime->shuttingDown) {
    return;
  }
  clearControlNotice(runtime->state);
  runtime->configurationResetPending = true;
  setControlOperationPending(runtime->state, true);
  render(runtime);
  if (!resetConfigurationAsync(runtime->configurationResetClient,
                               onConfigurationResetCompleted, runtime)) {
    runtime->configurationResetPending = false;
    setControlOperationPending(runtime->state, false);
    setControlDiagnostic(
        runtime->state,
        "PipeTune configuration reset could not be started");
    render(runtime);
  }
}

static void onConfigurationResetResponse(GtkDialog *dialog,
                                         gint responseId,
                                         gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  gtk_widget_destroy(GTK_WIDGET(dialog));
  if (responseId == GTK_RESPONSE_ACCEPT) {
    startConfigurationReset(runtime);
  }
}

static void onConfigurationResetClicked(GtkButton *,
                                        gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->configurationResetPending ||
      runtime->state.operationPending || runtime->shuttingDown) {
    return;
  }
  auto *dialog = gtk_message_dialog_new(
      GTK_WINDOW(runtime->ui.window),
      static_cast<GtkDialogFlags>(GTK_DIALOG_MODAL |
                                  GTK_DIALOG_DESTROY_WITH_PARENT),
      GTK_MESSAGE_WARNING, GTK_BUTTONS_NONE, "%s",
      "Reset all PipeTune configuration?");
  gtk_message_dialog_format_secondary_text(
      GTK_MESSAGE_DIALOG(dialog), "%s",
      "This selects Bypass, follows the system-default output, and uses "
      "Max + Suggest. The PipeTune service will restart if it is running.");
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Cancel",
                        GTK_RESPONSE_CANCEL);
  gtk_dialog_add_button(GTK_DIALOG(dialog), "Reset",
                        GTK_RESPONSE_ACCEPT);
  gtk_dialog_set_default_response(GTK_DIALOG(dialog),
                                  GTK_RESPONSE_CANCEL);
  g_signal_connect(dialog, "response",
                   G_CALLBACK(onConfigurationResetResponse), runtime);
  gtk_widget_show_all(dialog);
}

static void onOutputReply(const ControlClientReply &reply,
                          void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  const auto completion = completeOutputOperation(
      runtime->state, reply,
      {.configPath = runtime->startupConfigPath,
       .clearPreference = runtime->pendingOutputClear,
       .target = runtime->pendingOutputTarget},
      currentMonotonicMilliseconds());
  runtime->outputChangePending = false;
  runtime->pendingOutputClear = false;
  runtime->pendingOutputTarget.clear();
  if (completion.reconnectRequired) {
    scheduleReconnect(runtime);
  }
  render(runtime);
}

static void onOutputChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingOutputCombo || runtime->outputChangePending ||
      runtime->state.operationPending ||
      runtime->state.connection != ControlConnectionState::connected ||
      !runtime->state.hasRuntimeStatus ||
      runtime->controlClient == nullptr) {
    return;
  }
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >=
          runtime->outputChoices.size()) {
    return;
  }
  const auto &choice =
      runtime->outputChoices[static_cast<std::size_t>(selected)];
  if ((choice.clearPreference &&
       runtime->state.runtime.preferredTarget.empty()) ||
      (!choice.clearPreference &&
       choice.target == runtime->state.runtime.preferredTarget)) {
    return;
  }

  clearControlNotice(runtime->state);
  runtime->outputChangePending = true;
  runtime->pendingOutputClear = choice.clearPreference;
  runtime->pendingOutputTarget = choice.target;
  setControlOperationPending(runtime->state, true);
  render(runtime);
  if (choice.clearPreference) {
    clearControlOutputAsync(runtime->controlClient, onOutputReply,
                            runtime);
  } else {
    setControlOutputAsync(runtime->controlClient, choice.target,
                          onOutputReply, runtime);
  }
}

static void updateRateEditDirty(GtkRuntime *runtime) {
  runtime->rateEditDirty =
      runtime->editedRatePolicy != displayedRatePolicy(*runtime);
}

static void onRateChanged(GtkComboBox *combo, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingRateControls || runtime->rateChangePending ||
      runtime->state.operationPending) {
    return;
  }
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected < 0 ||
      static_cast<std::size_t>(selected) >=
          runtime->rateChoices.size()) {
    return;
  }
  const auto &choice =
      runtime->rateChoices[static_cast<std::size_t>(selected)];
  runtime->editedRatePolicy.mode = choice.mode;
  runtime->editedRatePolicy.fixedRate = choice.fixedRate;
  updateRateEditDirty(runtime);
  render(runtime);
}

static void onRateEnforcementChanged(GtkComboBox *combo,
                                     gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingRateControls || runtime->rateChangePending ||
      runtime->state.operationPending) {
    return;
  }
  const auto selected = gtk_combo_box_get_active(combo);
  if (selected != 0 && selected != 1) {
    return;
  }
  runtime->editedRatePolicy.enforcement =
      selected == 1 ? pipetune::SampleRateEnforcement::force
                    : pipetune::SampleRateEnforcement::suggest;
  updateRateEditDirty(runtime);
  render(runtime);
}

static void onRateReply(const ControlClientReply &reply,
                        void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  const auto completion = completeRateOperation(
      runtime->state, reply,
      {.configPath = runtime->startupConfigPath,
       .policy = runtime->pendingRatePolicy},
      currentMonotonicMilliseconds());
  runtime->rateChangePending = false;
  if (completion.persistenceApplied) {
    runtime->startupRatePolicy = runtime->pendingRatePolicy;
  }
  runtime->rateEditDirty = !completion.persistenceApplied;
  if (completion.liveApplied) {
    runtime->editedRatePolicy =
        reply.response.status.configuredRatePolicy;
  }
  if (completion.reconnectRequired) {
    scheduleReconnect(runtime);
  }
  render(runtime);
}

static void onRateApplyClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!runtime->rateEditDirty || runtime->rateChangePending ||
      runtime->state.operationPending ||
      !pipetune::sampleRatePolicyIsValid(runtime->editedRatePolicy)) {
    return;
  }
  clearControlNotice(runtime->state);
  runtime->pendingRatePolicy = runtime->editedRatePolicy;
  runtime->rateChangePending = true;

  if (runtime->state.connection !=
      ControlConnectionState::connected) {
    const auto completion = persistRateOperationForNextStart(
        runtime->state,
        {.configPath = runtime->startupConfigPath,
         .policy = runtime->pendingRatePolicy});
    runtime->rateChangePending = false;
    if (completion.persistenceApplied) {
      runtime->startupRatePolicy = runtime->pendingRatePolicy;
      runtime->rateEditDirty = false;
    }
    render(runtime);
    return;
  }

  setControlOperationPending(runtime->state, true);
  render(runtime);
  setControlRateAsync(runtime->controlClient,
                      runtime->pendingRatePolicy, onRateReply, runtime);
}

static std::string savePendingPreset(GtkRuntime *runtime) {
  if (runtime->startupConfigPath.empty()) {
    return "startup configuration path is unavailable";
  }
  const auto error =
      pipetune::saveStartupPreset(runtime->startupConfigPath,
                                  runtime->pendingPreset);
  if (error.empty()) {
    runtime->startupPreset = runtime->pendingPreset;
    runtime->hasStartupPreset = true;
  }
  return error;
}

static std::string saveStartupBypass(GtkRuntime *runtime) {
  if (runtime->startupConfigPath.empty()) {
    return "startup configuration path is unavailable";
  }
  const auto error =
      pipetune::clearStartupPreset(runtime->startupConfigPath);
  if (error.empty()) {
    runtime->startupPreset.clear();
    runtime->hasStartupPreset = false;
  }
  return error;
}

static void onLoadReply(const ControlClientReply &reply, void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  setControlOperationPending(runtime->state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(runtime->state, reply.transportError);
    scheduleReconnect(runtime);
    render(runtime);
    return;
  }
  applyControlResponse(runtime->state, reply.response,
                       currentMonotonicMilliseconds());
  if (!reply.response.valid || !reply.response.success) {
    render(runtime);
    return;
  }

  const auto saveError = savePendingPreset(runtime);
  if (!saveError.empty()) {
    setControlDiagnostic(
        runtime->state,
        "Preset was applied, but startup persistence failed: " +
            saveError);
  }
  render(runtime);
}

static void onApplyClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  auto *filename = gtk_file_chooser_get_filename(
      GTK_FILE_CHOOSER(runtime->ui.presetChooser));
  if (filename == nullptr) {
    setControlDiagnostic(runtime->state,
                         "Select an EffeTune preset first");
    render(runtime);
    return;
  }
  auto filesystemError = std::error_code{};
  auto selected =
      std::filesystem::absolute(filename, filesystemError).lexically_normal();
  g_free(filename);
  if (filesystemError) {
    setControlDiagnostic(runtime->state,
                         "Cannot resolve selected preset: " +
                             filesystemError.message());
    render(runtime);
    return;
  }
  runtime->pendingPreset = std::move(selected);
  clearControlNotice(runtime->state);

  if (runtime->state.connection !=
      ControlConnectionState::connected) {
    const auto error = savePendingPreset(runtime);
    if (!error.empty()) {
      setControlDiagnostic(runtime->state, error);
    }
    render(runtime);
    return;
  }

  setControlOperationPending(runtime->state, true);
  render(runtime);
  loadControlPresetAsync(runtime->controlClient, runtime->pendingPreset,
                         onLoadReply, runtime);
}

static void onBypassReply(const ControlClientReply &reply,
                          void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  setControlOperationPending(runtime->state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(runtime->state, reply.transportError);
    scheduleReconnect(runtime);
    const auto saveError = saveStartupBypass(runtime);
    if (saveError.empty()) {
      setControlDiagnostic(
          runtime->state,
          "Daemon disconnected; DSP bypass was saved for the next start");
    } else {
      setControlDiagnostic(
          runtime->state,
          "Daemon disconnected and startup bypass could not be saved: " +
              saveError);
    }
    render(runtime);
    return;
  }

  applyControlResponse(runtime->state, reply.response,
                       currentMonotonicMilliseconds());
  if (!reply.response.valid || !reply.response.success) {
    render(runtime);
    return;
  }
  if (reply.response.status.processingMode !=
          pipetune::ProcessingMode::bypass ||
      !reply.response.status.activePreset.empty()) {
    setControlDiagnostic(runtime->state,
                         "Daemon did not confirm DSP bypass");
    render(runtime);
    return;
  }
  const auto saveError = saveStartupBypass(runtime);
  if (!saveError.empty()) {
    setControlDiagnostic(
        runtime->state,
        "DSP bypass was applied, but startup persistence failed: " +
            saveError);
  }
  render(runtime);
}

static void onBypassClicked(GtkButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  clearControlNotice(runtime->state);
  if (runtime->state.connection !=
      ControlConnectionState::connected) {
    const auto error = saveStartupBypass(runtime);
    if (!error.empty()) {
      setControlDiagnostic(runtime->state, error);
    }
    render(runtime);
    return;
  }

  setControlOperationPending(runtime->state, true);
  render(runtime);
  bypassControlAsync(runtime->controlClient, onBypassReply, runtime);
}

static std::string presetChoiceLabel(const PresetChoice &choice) {
  if (choice.source == PresetSource::saved) {
    return "Saved in EffeTune · " + choice.name;
  }
  return "Standard · " + choice.category + " · " + choice.name;
}

static std::string catalogDiagnosticText(
    const std::vector<std::string> &diagnostics,
    std::string_view pathResolutionError) {
  auto text = std::string{};
  appendNotice(text, pathResolutionError);
  for (const auto &diagnostic : diagnostics) {
    appendNotice(text, diagnostic);
  }
  return text;
}

static void updatePresetCatalogDiagnostic(GtkRuntime *runtime) {
  runtime->presetCatalogDiagnostic =
      runtime->presetCatalogSourceDiagnostic;
  appendNotice(runtime->presetCatalogDiagnostic,
               runtime->presetCatalogSavedDiagnostic);
}

static void replacePresetChoices(
    GtkRuntime *runtime, std::vector<PresetChoice> choices) {
  auto preserveChoice = false;
  auto preservedSource = PresetSource::standard;
  auto preservedName = std::string{};
  auto preservedPreset = std::string{};
  const auto active =
      gtk_combo_box_get_active(GTK_COMBO_BOX(runtime->ui.presetCombo));
  if (active > 0 &&
      static_cast<std::size_t>(active - 1) <
          runtime->presetChoices.size()) {
    const auto &choice =
        runtime->presetChoices[static_cast<std::size_t>(active - 1)];
    preserveChoice = true;
    preservedSource = choice.source;
    preservedName = choice.name;
    preservedPreset = choice.serializedPreset;
  }
  runtime->presetChoices = std::move(choices);

  runtime->updatingPresetCombo = true;
  gtk_combo_box_text_remove_all(
      GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo));
  gtk_combo_box_text_append_text(
      GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo),
      "Choose a standard or saved EffeTune preset…");
  auto activeIndex = gint{0};
  for (auto index = std::size_t{0};
       index < runtime->presetChoices.size(); ++index) {
    const auto &choice = runtime->presetChoices[index];
    const auto label = presetChoiceLabel(choice);
    gtk_combo_box_text_append_text(
        GTK_COMBO_BOX_TEXT(runtime->ui.presetCombo), label.c_str());
    if ((preserveChoice && choice.source == preservedSource &&
         choice.name == preservedName &&
         (choice.source == PresetSource::standard ||
          choice.serializedPreset == preservedPreset)) ||
        (!preserveChoice && runtime->hasStartupPreset &&
         choice.source == PresetSource::standard &&
         choice.path == runtime->startupPreset)) {
      activeIndex = static_cast<gint>(index + 1);
    }
  }
  gtk_combo_box_set_active(GTK_COMBO_BOX(runtime->ui.presetCombo),
                           activeIndex);
  runtime->updatingPresetCombo = false;
  gtk_widget_set_sensitive(runtime->ui.presetCombo,
                           !runtime->presetChoices.empty());
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
  updatePresetCatalogDiagnostic(runtime);
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
  appendNotice(runtime->presetCatalogSourceDiagnostic,
               userPath.error);
  if (!userPath.error.empty()) {
    updatePresetCatalogDiagnostic(runtime);
    return;
  }

  runtime->effetuneUserPresetPath = userPath.path;
  refreshSavedPresetCatalog(runtime);
  const auto monitor = createEffeTunePresetFileMonitor(
      runtime->effetuneUserPresetPath,
      onEffeTunePresetFileChanged, runtime);
  runtime->presetFileMonitor = monitor.monitor;
  appendNotice(runtime->presetCatalogSourceDiagnostic,
               monitor.error);
  updatePresetCatalogDiagnostic(runtime);
}

static void onPresetComboChanged(GtkComboBox *combo,
                                 gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingPresetCombo) {
    return;
  }
  const auto active = gtk_combo_box_get_active(combo);
  if (active <= 0 ||
      static_cast<std::size_t>(active - 1) >=
          runtime->presetChoices.size()) {
    return;
  }
  if (runtime->startupConfigPath.empty()) {
    setControlDiagnostic(
        runtime->state,
        "startup configuration path is unavailable");
    render(runtime);
    return;
  }

  const auto &choice =
      runtime->presetChoices[static_cast<std::size_t>(active - 1)];
  const auto resolved = resolvePresetChoicePath(
      choice,
      runtime->startupConfigPath.parent_path() / "effetune-presets");
  if (!resolved.error.empty()) {
    setControlDiagnostic(runtime->state, resolved.error);
    render(runtime);
    return;
  }

  runtime->updatingPresetCombo = true;
  const auto selected = gtk_file_chooser_set_filename(
      GTK_FILE_CHOOSER(runtime->ui.presetChooser),
      resolved.path.c_str());
  runtime->updatingPresetCombo = false;
  if (!selected) {
    setControlDiagnostic(
        runtime->state,
        "Cannot select EffeTune preset: " + resolved.path.string());
    render(runtime);
    return;
  }
  clearControlNotice(runtime->state);
  render(runtime);
}

static void onPresetFileSet(GtkFileChooserButton *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (runtime->updatingPresetCombo) {
    return;
  }
  runtime->updatingPresetCombo = true;
  gtk_combo_box_set_active(GTK_COMBO_BOX(runtime->ui.presetCombo), 0);
  runtime->updatingPresetCombo = false;
}

static void connectMainWindowSignals(GtkRuntime *runtime) {
  g_signal_connect(runtime->ui.window, "delete-event",
                   G_CALLBACK(onWindowDelete), runtime);
  g_signal_connect(runtime->ui.window, "destroy",
                   G_CALLBACK(onWindowDestroy), runtime);
  g_signal_connect(runtime->ui.outputCombo, "changed",
                   G_CALLBACK(onOutputChanged), runtime);
  g_signal_connect(runtime->ui.rateCombo, "changed",
                   G_CALLBACK(onRateChanged), runtime);
  g_signal_connect(runtime->ui.rateEnforcementCombo, "changed",
                   G_CALLBACK(onRateEnforcementChanged), runtime);
  g_signal_connect(runtime->ui.rateApplyButton, "clicked",
                   G_CALLBACK(onRateApplyClicked), runtime);
  g_signal_connect(runtime->ui.presetCombo, "changed",
                   G_CALLBACK(onPresetComboChanged), runtime);
  g_signal_connect(runtime->ui.presetChooser, "file-set",
                   G_CALLBACK(onPresetFileSet), runtime);
  g_signal_connect(runtime->ui.applyButton, "clicked",
                   G_CALLBACK(onApplyClicked), runtime);
  g_signal_connect(runtime->ui.bypassButton, "clicked",
                   G_CALLBACK(onBypassClicked), runtime);
  g_signal_connect(runtime->ui.resetButton, "clicked",
                   G_CALLBACK(onConfigurationResetClicked), runtime);
  g_signal_connect(runtime->ui.dismissButton, "clicked",
                   G_CALLBACK(onNoticeDismiss), runtime);
}

static void presentWindow(GtkRuntime *runtime,
                          guint32 userInteractionTime) {
  if (runtime == nullptr || runtime->ui.window == nullptr) {
    return;
  }
  refreshSavedPresetCatalog(runtime);
  presentMainWindow(runtime->ui, userInteractionTime);
  const auto notice =
      noticeText(runtime->state, runtime->presetCatalogDiagnostic);
  gtk_widget_set_visible(runtime->ui.noticeBox, !notice.empty());
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
  runtime->trayAvailability = availability;
}

static std::string reloadStartupConfig(GtkRuntime *runtime,
                                       bool resetSelections) {
  if (runtime->startupConfigPath.empty()) {
    return "startup configuration path is unavailable";
  }
  const auto loaded =
      pipetune::loadStartupConfig(runtime->startupConfigPath);
  if (!loaded.error.empty()) {
    return loaded.error;
  }

  runtime->hasStartupPreset = loaded.presetFound;
  runtime->startupPreset = loaded.presetPath;
  runtime->startupRatePolicy = loaded.ratePolicy;
  runtime->editedRatePolicy = loaded.ratePolicy;
  runtime->pendingRatePolicy = loaded.ratePolicy;
  runtime->rateEditDirty = false;
  runtime->pendingPreset.clear();

  if (resetSelections) {
    gtk_file_chooser_unselect_all(
        GTK_FILE_CHOOSER(runtime->ui.presetChooser));
    runtime->updatingPresetCombo = true;
    gtk_combo_box_set_active(
        GTK_COMBO_BOX(runtime->ui.presetCombo), 0);
    runtime->updatingPresetCombo = false;
  }
  if (loaded.presetFound &&
      std::filesystem::exists(loaded.presetPath)) {
    gtk_file_chooser_set_filename(
        GTK_FILE_CHOOSER(runtime->ui.presetChooser),
        loaded.presetPath.c_str());
  }
  return {};
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
    return;
  }
  runtime->startupConfigPath = resolved.path;
  const auto error = reloadStartupConfig(runtime, false);
  if (!error.empty()) {
    setControlDiagnostic(runtime->state, error);
  }
}

static void initializeConfigurationResetClient(GtkRuntime *runtime) {
  runtime->configurationResetClient =
      createConfigurationResetClient(kPipeTuneExecutable);
  if (runtime->configurationResetClient == nullptr) {
    auto diagnostic = runtime->state.diagnostic;
    appendNotice(
        diagnostic,
        "installed PipeTune executable path is unavailable");
    setControlDiagnostic(runtime->state, diagnostic);
  }
}

static void initializeControlClient(GtkRuntime *runtime) {
  const auto socket = pipetune::resolveControlSocketPath({});
  if (!socket.error.empty()) {
    markControlDisconnected(runtime->state, socket.error);
    return;
  }
  runtime->controlClient = createControlClient(
      socket.path,
      {.message = onSubscriptionMessage,
       .connectionChanged = onConnectionChanged,
       .userData = runtime});
  markControlConnecting(runtime->state);
  startControlSubscription(runtime->controlClient);
}

static void onApplicationStartup(GApplication *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  runtime->ui = createMainWindowUi(
      runtime->application, pipetune::version(),
      pipetune::effetuneVersion());
  initializeStatusArtwork(runtime);
  connectMainWindowSignals(runtime);
  initializeStartupConfig(runtime);
  initializeConfigurationResetClient(runtime);
  initializePresetCatalog(runtime);
  g_application_hold(G_APPLICATION(runtime->application));
  runtime->applicationHeld = true;
  runtime->trayBackend = createTrayBackend({
      .application = G_APPLICATION(runtime->application),
      .identifier = "pipetune",
      .title = "PipeTune",
      .iconState = TrayIconState::disconnected,
      .colorMode = TrayIconColorMode::grayscale,
      .tooltip = "PipeTune: disconnected",
      .callbacks =
          {
              .activate =
                  [runtime](std::uint32_t userInteractionTime) {
                    presentWindow(runtime, userInteractionTime);
                  },
              .quit = [runtime]() { requestQuit(runtime); },
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
    requestQuit(runtime);
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
  destroyConfigurationResetClient(runtime->configurationResetClient);
  runtime->configurationResetClient = nullptr;
  runtime->configurationResetPending = false;
  destroyTrayBackend(runtime->trayBackend);
  runtime->trayBackend = nullptr;
  destroyMainWindowUi(runtime->ui);
  releaseStatusArtwork(runtime);
  releaseApplicationHold(runtime);
}

static int runApplication(int argc, char **argv) {
  auto *application = gtk_application_new(
      applicationId(), G_APPLICATION_HANDLES_COMMAND_LINE);
  auto runtime = GtkRuntime{
      .application = application,
      .state = initialApplicationState(),
      .controlClient = nullptr,
      .configurationResetClient = nullptr,
      .configurationResetPending = false,
      .trayBackend = nullptr,
      .trayAvailability = TrayBackendAvailabilityState::pending,
      .startupConfigPath = {},
      .startupPreset = {},
      .hasStartupPreset = false,
      .pendingPreset = {},
      .presetChoices = {},
      .effetuneUserPresetPath = {},
      .presetFileMonitor = nullptr,
      .updatingPresetCombo = false,
      .presetCatalogSourceDiagnostic = {},
      .presetCatalogSavedDiagnostic = {},
      .presetCatalogDiagnostic = {},
      .outputChoices = {},
      .updatingOutputCombo = false,
      .outputChangePending = false,
      .pendingOutputClear = false,
      .pendingOutputTarget = {},
      .startupRatePolicy = pipetune::defaultSampleRatePolicy(),
      .editedRatePolicy = pipetune::defaultSampleRatePolicy(),
      .pendingRatePolicy = pipetune::defaultSampleRatePolicy(),
      .rateChoices = {},
      .updatingRateControls = false,
      .rateEditDirty = false,
      .rateChangePending = false,
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
