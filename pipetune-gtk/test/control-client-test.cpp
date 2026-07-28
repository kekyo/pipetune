#include "control-client.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"

#include <gio/gio.h>

#include <filesystem>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <unistd.h>

struct ServerState {
  std::mutex mutex;
  std::string activePreset;
  bool bypassed;
  std::string preferredTarget;
};

static pipetune::ControlRuntimeStatus serverStatus(ServerState &state) {
  auto lock = std::scoped_lock(state.mutex);
  return {.processingMode = state.bypassed
                                ? pipetune::ProcessingMode::bypass
                                : pipetune::ProcessingMode::preset,
          .activePreset = state.bypassed ? std::string{}
                                         : state.activePreset,
          .configurationError = {},
          .activePluginCount = state.bypassed ? 0u : 3u,
          .preferredTarget = state.preferredTarget,
          .selectedTarget = state.preferredTarget.empty()
                                ? std::string("alsa_output.test")
                                : state.preferredTarget,
          .outputSelectionReason =
              state.preferredTarget.empty()
                  ? pipetune::ControlOutputSelectionReason::systemDefault
                  : pipetune::ControlOutputSelectionReason::preferred,
          .availableOutputs =
              {{.name = "alsa_output.test",
                .description = "Test Output",
                .systemDefault = true,
                .preferred = false,
                .selected = state.preferredTarget.empty()},
               {.name = "alsa_output.headphones",
                .description = "Test Headphones",
                .systemDefault = false,
                .preferred =
                    state.preferredTarget == "alsa_output.headphones",
                .selected =
                    state.preferredTarget == "alsa_output.headphones"}},
          .defaultSinkActive = true,
          .overrunFrames = 0,
          .underrunFrames = 0,
          .processingErrors = 0,
          .dspProcessedFrames = 0,
          .dspProcessingNanoseconds = 0,
          .inputSampleFormat = {},
          .inputSampleRate = 0,
          .inputChannelCount = 0,
          .inputFramesReceived = 0,
          .inputLastReceivedUnixMilliseconds = 0};
}

static std::string provideStatus(void *userData) {
  auto &state = *static_cast<ServerState *>(userData);
  return pipetune::makeControlStatusEvent(serverStatus(state));
}

static pipetune::ControlMessageResult handleRequest(
    std::string_view message, void *userData) {
  auto &state = *static_cast<ServerState *>(userData);
  const auto request = pipetune::parseControlRequest(message);
  if (!request.error.empty()) {
    return {.response = pipetune::makeControlErrorResponse(request.error),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = false};
  }
  if (request.request.command == pipetune::ControlCommand::subscribe) {
    return {.response = provideStatus(&state),
            .connectionMode = pipetune::ControlConnectionMode::subscribe,
            .publishStatus = false};
  }
  if (request.request.command == pipetune::ControlCommand::loadPreset) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.activePreset = request.request.presetPath.string();
      state.bypassed = false;
    }
    return {.response = pipetune::makeControlSuccessResponse(
                serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = true};
  }
  if (request.request.command == pipetune::ControlCommand::bypass) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.activePreset.clear();
      state.bypassed = true;
    }
    return {.response = pipetune::makeControlSuccessResponse(
                serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = true};
  }
  if (request.request.command == pipetune::ControlCommand::setOutput) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.preferredTarget = request.request.outputTarget;
    }
    return {.response = pipetune::makeControlSuccessResponse(
                serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = true};
  }
  if (request.request.command == pipetune::ControlCommand::clearOutput) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.preferredTarget.clear();
    }
    return {.response = pipetune::makeControlSuccessResponse(
                serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = true};
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = false};
}

struct ClientTestState {
  GMainLoop *loop;
  pipetune_gtk::ControlClient *client;
  std::unique_ptr<pipetune::ControlServer> *server;
  bool connected;
  bool initialStatus;
  bool loadReply;
  bool publishedStatus;
  bool bypassReply;
  bool publishedBypass;
  bool setOutputRequested;
  bool setOutputReply;
  bool publishedSetOutput;
  bool clearOutputRequested;
  bool clearOutputReply;
  bool publishedClearOutput;
  bool disconnected;
  bool timedOut;
  bool failed;
};

static gboolean stopServer(gpointer userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  state.server->reset();
  return G_SOURCE_REMOVE;
}

static void maybeStopServer(ClientTestState &state) {
  if (state.loadReply && state.publishedStatus && state.bypassReply &&
      state.publishedBypass && state.setOutputReply &&
      state.publishedSetOutput && state.clearOutputReply &&
      state.publishedClearOutput && *state.server != nullptr) {
    g_idle_add(stopServer, &state);
  }
}

static void maybeStartClearOutput(ClientTestState &state);

static void onSetOutputReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      reply.response.status.preferredTarget !=
          "alsa_output.headphones" ||
      reply.response.status.selectedTarget !=
          "alsa_output.headphones") {
    state.failed = true;
  } else {
    state.setOutputReply = true;
  }
  maybeStartClearOutput(state);
}

static void onClearOutputReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      !reply.response.status.preferredTarget.empty() ||
      reply.response.status.outputSelectionReason !=
          pipetune::ControlOutputSelectionReason::systemDefault) {
    state.failed = true;
  } else {
    state.clearOutputReply = true;
  }
  maybeStopServer(state);
}

static void maybeStartClearOutput(ClientTestState &state) {
  if (state.setOutputReply && state.publishedSetOutput &&
      !state.clearOutputRequested) {
    state.clearOutputRequested = true;
    pipetune_gtk::clearControlOutputAsync(
        state.client, onClearOutputReply, &state);
  }
  maybeStopServer(state);
}

static void maybeStartSetOutput(ClientTestState &state) {
  if (state.bypassReply && state.publishedBypass &&
      !state.setOutputRequested) {
    state.setOutputRequested = true;
    pipetune_gtk::setControlOutputAsync(
        state.client, "alsa_output.headphones", onSetOutputReply, &state);
  }
  maybeStopServer(state);
}

static void onBypassReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      reply.response.status.processingMode !=
          pipetune::ProcessingMode::bypass ||
      !reply.response.status.activePreset.empty()) {
    state.failed = true;
  } else {
    state.bypassReply = true;
  }
  maybeStartSetOutput(state);
}

static void onLoadReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      reply.response.status.activePreset !=
          "/tmp/selected.effetune_preset") {
    state.failed = true;
  } else {
    state.loadReply = true;
  }
  maybeStopServer(state);
}

static void onMessage(
    const pipetune::ControlResponseParseResult &message, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!message.valid || !message.success ||
      message.kind != pipetune::ControlResponseKind::statusEvent) {
    state.failed = true;
    return;
  }
  if (message.status.activePreset == "/tmp/initial.effetune_preset") {
    if (!state.initialStatus) {
      state.initialStatus = true;
      pipetune_gtk::loadControlPresetAsync(
          state.client, "/tmp/selected.effetune_preset", onLoadReply,
          &state);
    }
  } else if (message.status.activePreset ==
             "/tmp/selected.effetune_preset") {
    if (!state.publishedStatus) {
      state.publishedStatus = true;
      pipetune_gtk::bypassControlAsync(
          state.client, onBypassReply, &state);
    }
  } else if (message.status.processingMode ==
                 pipetune::ProcessingMode::bypass &&
             message.status.activePreset.empty() &&
             message.status.preferredTarget ==
                 "alsa_output.headphones") {
    state.publishedSetOutput = true;
    maybeStartClearOutput(state);
    return;
  } else if (message.status.processingMode ==
                 pipetune::ProcessingMode::bypass &&
             message.status.activePreset.empty() &&
             message.status.preferredTarget.empty()) {
    if (!state.setOutputRequested) {
      state.publishedBypass = true;
      maybeStartSetOutput(state);
      return;
    }
    if (state.clearOutputRequested) {
      state.publishedClearOutput = true;
    }
  } else {
    state.failed = true;
  }
  maybeStopServer(state);
}

static void onConnectionChanged(bool connected, std::string_view error,
                                void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (connected) {
    state.connected = true;
    return;
  }
  if (state.connected && !error.empty()) {
    state.disconnected = true;
    g_main_loop_quit(state.loop);
  }
}

static gboolean onTimeout(gpointer userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  state.timedOut = true;
  g_main_loop_quit(state.loop);
  return G_SOURCE_REMOVE;
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-gtk-client-test-" +
       std::to_string(static_cast<long long>(getpid())));
  const auto socketPath = directory / "control.sock";
  auto serverState =
      ServerState{.mutex = {},
                  .activePreset = "/tmp/initial.effetune_preset",
                  .bypassed = false,
                  .preferredTarget = {}};
  auto started = pipetune::startControlServer(
      socketPath,
      {.handler = handleRequest,
       .statusProvider = provideStatus,
       .userData = &serverState});
  if (started.server == nullptr) {
    std::cerr << started.error << '\n';
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto *loop = g_main_loop_new(nullptr, FALSE);
  auto state = ClientTestState{
      .loop = loop,
      .client = nullptr,
      .server = &started.server,
      .connected = false,
      .initialStatus = false,
      .loadReply = false,
      .publishedStatus = false,
      .bypassReply = false,
      .publishedBypass = false,
      .setOutputRequested = false,
      .setOutputReply = false,
      .publishedSetOutput = false,
      .clearOutputRequested = false,
      .clearOutputReply = false,
      .publishedClearOutput = false,
      .disconnected = false,
      .timedOut = false,
      .failed = false,
  };
  state.client = pipetune_gtk::createControlClient(
      socketPath,
      {.message = onMessage,
       .connectionChanged = onConnectionChanged,
       .userData = &state});
  pipetune_gtk::startControlSubscription(state.client);
  const auto timeout = g_timeout_add_seconds(5, onTimeout, &state);
  g_main_loop_run(loop);
  if (!state.timedOut) {
    g_source_remove(timeout);
  }
  pipetune_gtk::destroyControlClient(state.client);
  started.server.reset();
  g_main_loop_unref(loop);
  std::filesystem::remove_all(directory);

  if (state.failed || state.timedOut || !state.connected ||
      !state.initialStatus || !state.loadReply ||
      !state.publishedStatus || !state.bypassReply ||
      !state.publishedBypass || !state.setOutputReply ||
      !state.publishedSetOutput || !state.clearOutputReply ||
      !state.publishedClearOutput || !state.disconnected) {
    std::cerr << "asynchronous control client lifecycle differs\n";
    return 1;
  }
  return 0;
}
