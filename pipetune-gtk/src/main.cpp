#include "application-state.h"
#include "control-client.h"
#include "launch-options.h"
#include "tray-backend.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"
#include "pipetune/version.h"

#include <gtk/gtk.h>

#include <cstdlib>
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
  guint reconnectSource;
  bool applicationHeld;
  bool activationHandled;
  bool startedHidden;
  bool shuttingDown;
  bool quitting;
  GtkWidget *window;
  GtkWidget *statusImage;
  GtkWidget *statusLabel;
  GtkWidget *processingModeLabel;
  GtkWidget *activePresetLabel;
  GtkWidget *startupPresetLabel;
  GtkWidget *pluginCountLabel;
  GtkWidget *targetLabel;
  GtkWidget *defaultSinkLabel;
  GtkWidget *counterLabel;
  GtkWidget *noticeBox;
  GtkWidget *noticeLabel;
  GtkWidget *presetChooser;
  GtkWidget *applyButton;
  GtkWidget *bypassButton;
  GtkWidget *refreshButton;
};

static std::string pathText(const std::filesystem::path &path) {
  return path.empty() ? std::string("—") : path.string();
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

static TrayIconColorMode iconColorModeForApplication(
    const ApplicationState &state) {
  return isPresetApplied(state) ? TrayIconColorMode::color
                                : TrayIconColorMode::grayscale;
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

static void render(GtkRuntime *runtime) {
  if (runtime == nullptr || runtime->window == nullptr) {
    return;
  }
  const auto status = connectionText(runtime->state);
  gtk_label_set_text(GTK_LABEL(runtime->statusLabel), status.c_str());
  const auto statusIcon =
      runtime->state.connection == ControlConnectionState::connected
          ? (trayVisualState(runtime->state) == TrayVisualState::active
                 ? "emblem-ok-symbolic"
                 : "dialog-warning-symbolic")
          : "network-offline-symbolic";
  gtk_image_set_from_icon_name(GTK_IMAGE(runtime->statusImage), statusIcon,
                               GTK_ICON_SIZE_DIALOG);

  const auto processingMode =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.processingMode ==
                     pipetune::ProcessingMode::bypass
                 ? "Bypass"
                 : "Preset")
          : "—";
  gtk_label_set_text(GTK_LABEL(runtime->processingModeLabel),
                     processingMode);
  const auto activePreset =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.processingMode ==
                     pipetune::ProcessingMode::bypass
                 ? std::string("None — pass-through")
                 : pathText(runtime->state.runtime.activePreset))
          : std::string("—");
  gtk_label_set_text(GTK_LABEL(runtime->activePresetLabel),
                     activePreset.c_str());
  const auto startupPreset =
      runtime->hasStartupPreset ? pathText(runtime->startupPreset)
                                : std::string("Bypass");
  gtk_label_set_text(GTK_LABEL(runtime->startupPresetLabel),
                     startupPreset.c_str());
  const auto pluginCount =
      runtime->state.hasRuntimeStatus
          ? std::to_string(runtime->state.runtime.activePluginCount)
          : std::string("—");
  gtk_label_set_text(GTK_LABEL(runtime->pluginCountLabel),
                     pluginCount.c_str());
  const auto target =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.selectedTarget.empty()
                 ? std::string("Unavailable")
                 : runtime->state.runtime.selectedTarget)
          : std::string("—");
  gtk_label_set_text(GTK_LABEL(runtime->targetLabel), target.c_str());
  const auto defaultSink =
      runtime->state.hasRuntimeStatus
          ? (runtime->state.runtime.defaultSinkActive ? "Active"
                                                      : "Inactive")
          : "—";
  gtk_label_set_text(GTK_LABEL(runtime->defaultSinkLabel), defaultSink);
  const auto counters =
      runtime->state.hasRuntimeStatus
          ? "Overrun " +
                std::to_string(runtime->state.runtime.overrunFrames) +
                "  •  Underrun " +
                std::to_string(runtime->state.runtime.underrunFrames) +
                "  •  Processing " +
                std::to_string(runtime->state.runtime.processingErrors)
          : std::string("—");
  gtk_label_set_text(GTK_LABEL(runtime->counterLabel), counters.c_str());

  const auto notice = noticeText(runtime->state);
  gtk_label_set_text(GTK_LABEL(runtime->noticeLabel), notice.c_str());
  gtk_widget_set_visible(runtime->noticeBox, !notice.empty());
  gtk_button_set_label(
      GTK_BUTTON(runtime->applyButton),
      runtime->state.connection == ControlConnectionState::connected
          ? "Apply and Save"
          : "Save for Next Start");
  gtk_widget_set_sensitive(runtime->applyButton,
                           !runtime->state.operationPending);
  gtk_button_set_label(
      GTK_BUTTON(runtime->bypassButton),
      runtime->state.connection == ControlConnectionState::connected
          ? "Bypass and Save"
          : "Save Bypass");
  gtk_widget_set_sensitive(runtime->bypassButton,
                           !runtime->state.operationPending);
  gtk_widget_set_sensitive(
      runtime->refreshButton,
      runtime->controlClient != nullptr &&
          !runtime->state.operationPending);
  updateTrayBackend(runtime->trayBackend,
                    iconStateForApplication(runtime->state),
                    iconColorModeForApplication(runtime->state),
                    trayTooltip(runtime->state));
}

static GtkWidget *addDetailRow(GtkGrid *grid, int row,
                               const char *title) {
  auto *titleLabel = gtk_label_new(title);
  gtk_label_set_xalign(GTK_LABEL(titleLabel), 0.0F);
  gtk_style_context_add_class(
      gtk_widget_get_style_context(titleLabel), "dim-label");
  gtk_grid_attach(grid, titleLabel, 0, row, 1, 1);

  auto *valueLabel = gtk_label_new("—");
  gtk_label_set_xalign(GTK_LABEL(valueLabel), 0.0F);
  gtk_label_set_selectable(GTK_LABEL(valueLabel), TRUE);
  gtk_label_set_ellipsize(GTK_LABEL(valueLabel), PANGO_ELLIPSIZE_MIDDLE);
  gtk_widget_set_hexpand(valueLabel, TRUE);
  gtk_grid_attach(grid, valueLabel, 1, row, 1, 1);
  return valueLabel;
}

static void presentWindow(GtkRuntime *runtime);
static void requestQuit(GtkRuntime *runtime);

static gboolean onWindowDelete(GtkWidget *, GdkEvent *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!runtime->quitting &&
      runtime->trayAvailability ==
          TrayBackendAvailabilityState::available) {
    gtk_widget_hide(runtime->window);
    return TRUE;
  }
  requestQuit(runtime);
  return TRUE;
}

static void onWindowDestroy(GtkWidget *, gpointer userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  runtime->window = nullptr;
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
  applyControlResponse(runtime->state, message);
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

static void onStatusReply(const ControlClientReply &reply,
                          void *userData) {
  auto *runtime = static_cast<GtkRuntime *>(userData);
  if (!reply.transportError.empty()) {
    markControlDisconnected(runtime->state, reply.transportError);
    scheduleReconnect(runtime);
  } else {
    applyControlResponse(runtime->state, reply.response);
  }
  render(runtime);
}

static void requestFreshStatus(GtkRuntime *runtime) {
  if (runtime->controlClient != nullptr &&
      !runtime->state.operationPending) {
    requestControlStatusAsync(runtime->controlClient, onStatusReply,
                              runtime);
  }
}

static void onRefreshClicked(GtkButton *, gpointer userData) {
  requestFreshStatus(static_cast<GtkRuntime *>(userData));
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
  applyControlResponse(runtime->state, reply.response);
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
      GTK_FILE_CHOOSER(runtime->presetChooser));
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

  applyControlResponse(runtime->state, reply.response);
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

static GtkWidget *createMainWindow(GtkRuntime *runtime) {
  auto *window = gtk_application_window_new(runtime->application);
  gtk_window_set_title(GTK_WINDOW(window), "PipeTune");
  gtk_window_set_default_size(GTK_WINDOW(window), 680, 470);
  gtk_window_set_position(GTK_WINDOW(window), GTK_WIN_POS_CENTER);

  auto *header = gtk_header_bar_new();
  gtk_header_bar_set_title(GTK_HEADER_BAR(header), "PipeTune");
  gtk_header_bar_set_subtitle(GTK_HEADER_BAR(header),
                              "PipeWire DSP control");
  gtk_header_bar_set_show_close_button(GTK_HEADER_BAR(header), TRUE);
  runtime->refreshButton =
      gtk_button_new_from_icon_name("view-refresh-symbolic",
                                    GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(runtime->refreshButton, "Refresh status");
  gtk_header_bar_pack_end(GTK_HEADER_BAR(header), runtime->refreshButton);
  gtk_window_set_titlebar(GTK_WINDOW(window), header);

  auto *root = gtk_box_new(GTK_ORIENTATION_VERTICAL, 18);
  gtk_container_set_border_width(GTK_CONTAINER(root), 20);
  gtk_container_add(GTK_CONTAINER(window), root);

  auto *statusBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
  runtime->statusImage =
      gtk_image_new_from_icon_name("network-offline-symbolic",
                                   GTK_ICON_SIZE_DIALOG);
  runtime->statusLabel = gtk_label_new("PipeTune is disconnected");
  gtk_label_set_xalign(GTK_LABEL(runtime->statusLabel), 0.0F);
  gtk_widget_set_hexpand(runtime->statusLabel, TRUE);
  gtk_box_pack_start(GTK_BOX(statusBox), runtime->statusImage, FALSE, FALSE,
                     0);
  gtk_box_pack_start(GTK_BOX(statusBox), runtime->statusLabel, TRUE, TRUE, 0);
  gtk_box_pack_start(GTK_BOX(root), statusBox, FALSE, FALSE, 0);

  auto *grid = gtk_grid_new();
  gtk_grid_set_column_spacing(GTK_GRID(grid), 18);
  gtk_grid_set_row_spacing(GTK_GRID(grid), 10);
  runtime->processingModeLabel =
      addDetailRow(GTK_GRID(grid), 0, "DSP mode");
  runtime->activePresetLabel =
      addDetailRow(GTK_GRID(grid), 1, "Active preset");
  runtime->startupPresetLabel =
      addDetailRow(GTK_GRID(grid), 2, "Startup preset");
  runtime->pluginCountLabel =
      addDetailRow(GTK_GRID(grid), 3, "Active DSP nodes");
  runtime->targetLabel =
      addDetailRow(GTK_GRID(grid), 4, "Output target");
  runtime->defaultSinkLabel =
      addDetailRow(GTK_GRID(grid), 5, "Default sink");
  runtime->counterLabel =
      addDetailRow(GTK_GRID(grid), 6, "Runtime counters");
  gtk_box_pack_start(GTK_BOX(root), grid, FALSE, FALSE, 0);

  runtime->noticeBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  gtk_style_context_add_class(
      gtk_widget_get_style_context(runtime->noticeBox), "warning");
  runtime->noticeLabel = gtk_label_new("");
  gtk_label_set_xalign(GTK_LABEL(runtime->noticeLabel), 0.0F);
  gtk_label_set_line_wrap(GTK_LABEL(runtime->noticeLabel), TRUE);
  gtk_widget_set_hexpand(runtime->noticeLabel, TRUE);
  auto *dismiss =
      gtk_button_new_from_icon_name("window-close-symbolic",
                                    GTK_ICON_SIZE_BUTTON);
  gtk_widget_set_tooltip_text(dismiss, "Dismiss");
  gtk_box_pack_start(GTK_BOX(runtime->noticeBox), runtime->noticeLabel,
                     TRUE, TRUE, 0);
  gtk_box_pack_end(GTK_BOX(runtime->noticeBox), dismiss, FALSE, FALSE, 0);
  gtk_box_pack_start(GTK_BOX(root), runtime->noticeBox, FALSE, FALSE, 0);

  auto *separator = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
  gtk_box_pack_start(GTK_BOX(root), separator, FALSE, FALSE, 0);
  auto *presetTitle = gtk_label_new(nullptr);
  gtk_label_set_markup(GTK_LABEL(presetTitle), "<b>Preset selection</b>");
  gtk_label_set_xalign(GTK_LABEL(presetTitle), 0.0F);
  gtk_box_pack_start(GTK_BOX(root), presetTitle, FALSE, FALSE, 0);
  auto *presetBox = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
  runtime->presetChooser = gtk_file_chooser_button_new(
      "Select an EffeTune preset", GTK_FILE_CHOOSER_ACTION_OPEN);
  auto *filter = gtk_file_filter_new();
  gtk_file_filter_set_name(filter, "EffeTune presets");
  gtk_file_filter_add_pattern(filter, "*.effetune_preset");
  gtk_file_chooser_add_filter(GTK_FILE_CHOOSER(runtime->presetChooser),
                              filter);
  gtk_widget_set_hexpand(runtime->presetChooser, TRUE);
  runtime->applyButton = gtk_button_new_with_label("Save for Next Start");
  runtime->bypassButton = gtk_button_new_with_label("Save Bypass");
  gtk_box_pack_start(GTK_BOX(presetBox), runtime->presetChooser, TRUE, TRUE,
                     0);
  gtk_box_pack_end(GTK_BOX(presetBox), runtime->applyButton, FALSE, FALSE, 0);
  gtk_box_pack_end(GTK_BOX(presetBox), runtime->bypassButton, FALSE, FALSE,
                   0);
  gtk_box_pack_start(GTK_BOX(root), presetBox, FALSE, FALSE, 0);

  g_signal_connect(window, "delete-event", G_CALLBACK(onWindowDelete),
                   runtime);
  g_signal_connect(window, "destroy", G_CALLBACK(onWindowDestroy), runtime);
  g_signal_connect(runtime->refreshButton, "clicked",
                   G_CALLBACK(onRefreshClicked), runtime);
  g_signal_connect(runtime->applyButton, "clicked",
                   G_CALLBACK(onApplyClicked), runtime);
  g_signal_connect(runtime->bypassButton, "clicked",
                   G_CALLBACK(onBypassClicked), runtime);
  g_signal_connect(dismiss, "clicked", G_CALLBACK(onNoticeDismiss), runtime);
  return window;
}

static void presentWindow(GtkRuntime *runtime) {
  if (runtime == nullptr || runtime->window == nullptr) {
    return;
  }
  gtk_widget_show_all(runtime->window);
  const auto notice = noticeText(runtime->state);
  gtk_widget_set_visible(runtime->noticeBox, !notice.empty());
  gtk_window_present(GTK_WINDOW(runtime->window));
  requestFreshStatus(runtime);
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
        GTK_FILE_CHOOSER(runtime->presetChooser),
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
  runtime->window = createMainWindow(runtime);
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
  if (runtime->window != nullptr) {
    gtk_widget_destroy(runtime->window);
  }
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
      .reconnectSource = 0,
      .applicationHeld = false,
      .activationHandled = false,
      .startedHidden = false,
      .shuttingDown = false,
      .quitting = false,
      .window = nullptr,
      .statusImage = nullptr,
      .statusLabel = nullptr,
      .processingModeLabel = nullptr,
      .activePresetLabel = nullptr,
      .startupPresetLabel = nullptr,
      .pluginCountLabel = nullptr,
      .targetLabel = nullptr,
      .defaultSinkLabel = nullptr,
      .counterLabel = nullptr,
      .noticeBox = nullptr,
      .noticeLabel = nullptr,
      .presetChooser = nullptr,
      .applyButton = nullptr,
      .bypassButton = nullptr,
      .refreshButton = nullptr,
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
    std::cout << "pipetune-gtk " << pipetune::version() << '\n';
    return 0;
  }
  return pipetune_gtk::runApplication(argc, argv);
}
