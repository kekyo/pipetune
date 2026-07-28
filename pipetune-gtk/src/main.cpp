#include "application-state.h"
#include "control-client.h"
#include "launch-options.h"
#include "main-window.h"
#include "output-operation.h"
#include "output-selection-model.h"
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
  TrayBackendState *trayBackend;
  TrayBackendAvailabilityState trayAvailability;
  std::filesystem::path startupConfigPath;
  std::filesystem::path startupPreset;
  bool hasStartupPreset;
  std::filesystem::path pendingPreset;
  std::vector<OutputDeviceChoice> outputChoices;
  bool updatingOutputCombo;
  bool outputChangePending;
  bool pendingOutputClear;
  std::string pendingOutputTarget;
  guint reconnectSource;
  bool applicationHeld;
  bool activationHandled;
  bool startedHidden;
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

static std::string noticeText(const ApplicationState &state) {
  auto notice = state.diagnostic;
  if (state.hasRuntimeStatus &&
      !state.runtime.configurationError.empty()) {
    if (!notice.empty()) {
      notice.push_back('\n');
    }
    notice += "Startup configuration: " +
              state.runtime.configurationError;
  }
  for (const auto &warning : state.warnings) {
    if (!notice.empty()) {
      notice.push_back('\n');
    }
    notice += "Preset node " + std::to_string(warning.nodeIndex + 1) +
              " (\"" + warning.pluginName + "\") was skipped: " +
              warning.reason;
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

  const auto notice = noticeText(runtime->state);
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
  updateTrayBackend(runtime->trayBackend,
                    iconStateForApplication(runtime->state),
                    iconPresentation.colorMode,
                    trayTooltip(runtime->state));
}

static void presentWindow(GtkRuntime *runtime);
static void requestQuit(GtkRuntime *runtime);

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

static void connectMainWindowSignals(GtkRuntime *runtime) {
  g_signal_connect(runtime->ui.window, "delete-event",
                   G_CALLBACK(onWindowDelete), runtime);
  g_signal_connect(runtime->ui.window, "destroy",
                   G_CALLBACK(onWindowDestroy), runtime);
  g_signal_connect(runtime->ui.outputCombo, "changed",
                   G_CALLBACK(onOutputChanged), runtime);
  g_signal_connect(runtime->ui.applyButton, "clicked",
                   G_CALLBACK(onApplyClicked), runtime);
  g_signal_connect(runtime->ui.bypassButton, "clicked",
                   G_CALLBACK(onBypassClicked), runtime);
  g_signal_connect(runtime->ui.dismissButton, "clicked",
                   G_CALLBACK(onNoticeDismiss), runtime);
}

static void presentWindow(GtkRuntime *runtime) {
  if (runtime == nullptr || runtime->ui.window == nullptr) {
    return;
  }
  gtk_widget_show_all(runtime->ui.window);
  const auto notice = noticeText(runtime->state);
  gtk_widget_set_visible(runtime->ui.noticeBox, !notice.empty());
  gtk_window_present(GTK_WINDOW(runtime->ui.window));
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
  if (availability == TrayBackendAvailabilityState::unavailable &&
      runtime->activationHandled && runtime->startedHidden) {
    presentWindow(runtime);
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
    return;
  }
  runtime->startupConfigPath = resolved.path;
  const auto loaded = pipetune::loadStartupPreset(runtime->startupConfigPath);
  if (!loaded.error.empty()) {
    setControlDiagnostic(runtime->state, loaded.error);
    return;
  }
  runtime->hasStartupPreset = loaded.found;
  runtime->startupPreset = loaded.presetPath;
  if (loaded.found && std::filesystem::exists(loaded.presetPath)) {
    gtk_file_chooser_set_filename(
        GTK_FILE_CHOOSER(runtime->ui.presetChooser),
        loaded.presetPath.c_str());
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
  runtime->ui =
      createMainWindowUi(runtime->application, versionText());
  initializeStatusArtwork(runtime);
  connectMainWindowSignals(runtime);
  initializeStartupConfig(runtime);
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
              .activate = [runtime]() { presentWindow(runtime); },
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
    runtime->startedHidden = parsed.options.hidden;
    if (!parsed.options.hidden ||
        runtime->trayAvailability ==
            TrayBackendAvailabilityState::unavailable) {
      presentWindow(runtime);
    }
    return 0;
  }
  if (!parsed.options.hidden) {
    presentWindow(runtime);
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
  destroyControlClient(runtime->controlClient);
  runtime->controlClient = nullptr;
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
      .trayBackend = nullptr,
      .trayAvailability = TrayBackendAvailabilityState::pending,
      .startupConfigPath = {},
      .startupPreset = {},
      .hasStartupPreset = false,
      .pendingPreset = {},
      .outputChoices = {},
      .updatingOutputCombo = false,
      .outputChangePending = false,
      .pendingOutputClear = false,
      .pendingOutputTarget = {},
      .reconnectSource = 0,
      .applicationHeld = false,
      .activationHandled = false,
      .startedHidden = false,
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
