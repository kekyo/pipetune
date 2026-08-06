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
  pipetune::SampleRatePolicy ratePolicy;
  pipetune::DspBackendKind dspBackend;
  pipetune::DspSimdVariant dspSimdVariant;
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
          .overrunFrames = 0,
          .underrunFrames = 0,
          .processingErrors = 0,
          .dspProcessedFrames = 0,
          .dspProcessingNanoseconds = 0,
          .inputSampleFormat = {},
          .inputSampleRate = 0,
          .inputChannelCount = 0,
          .inputFramesReceived = 0,
          .inputLastReceivedUnixMilliseconds = 0,
          .configuredRatePolicy = state.ratePolicy,
          .dspSampleRate =
              state.ratePolicy.mode == pipetune::SampleRateMode::automatic
                  ? 96000u
                  : state.ratePolicy.fixedRate,
          .graphSampleRate =
              state.ratePolicy.mode == pipetune::SampleRateMode::automatic
                  ? 96000u
                  : state.ratePolicy.fixedRate,
          .rateTransitioning = false,
          .rateError = {},
          .configuredDspBackend = state.dspBackend,
          .configuredDspSimdVariant = state.dspSimdVariant,
          .effectiveDspBackend = state.dspBackend,
          .effectiveDspVariant =
              state.dspBackend == pipetune::DspBackendKind::scalar
                  ? pipetune::DspBackendVariant::scalar
                  : pipetune::DspBackendVariant::simdBaseline,
          .dspBackendFallback = false,
          .dspBackendError = {},
          .availableDspBackends =
              {{
                  {.kind = pipetune::DspBackendKind::scalar,
                   .available = true,
                   .cpuRequirement = "none",
                   .error = {}},
                  {.kind = pipetune::DspBackendKind::simd,
                   .available = true,
                   .cpuRequirement = "test SIMD ISA",
                   .error = {}},
              }},
          .availableDspVariants =
              {{.variant = pipetune::DspBackendVariant::scalar,
                .available = true,
                .cpuSupported = true,
                .cpuRequirement = "none",
                .error = {}},
               {.variant =
                    pipetune::DspBackendVariant::simdBaseline,
                .available = true,
                .cpuSupported = true,
                .cpuRequirement = "test SIMD ISA",
                .error = {}}}};
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
  if (request.request.command == pipetune::ControlCommand::setRate) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.ratePolicy = request.request.ratePolicy;
    }
    return {.response = pipetune::makeControlSuccessResponse(
                serverStatus(state), {}),
            .connectionMode = pipetune::ControlConnectionMode::close,
            .publishStatus = true};
  }
  if (request.request.command ==
      pipetune::ControlCommand::setDspBackend) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.dspBackend = request.request.dspBackend;
      state.dspSimdVariant = request.request.dspSimdVariant;
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
  bool setRateRequested;
  bool setRateReply;
  bool publishedSetRate;
  bool setDspBackendRequested;
  bool setDspBackendReply;
  bool publishedSetDspBackend;
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
      state.publishedBypass && state.setRateReply &&
      state.publishedSetRate && state.setDspBackendReply &&
      state.publishedSetDspBackend && *state.server != nullptr) {
    g_idle_add(stopServer, &state);
  }
}

static void maybeStartSetRate(ClientTestState &state);
static void maybeStartSetDspBackend(ClientTestState &state);

static void onSetRateReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  const auto expected = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      reply.response.status.configuredRatePolicy != expected ||
      reply.response.status.dspSampleRate != 192000 ||
      reply.response.status.graphSampleRate != 192000) {
    state.failed = true;
  } else {
    state.setRateReply = true;
  }
  maybeStartSetDspBackend(state);
}

static void onSetDspBackendReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      reply.response.status.configuredDspBackend !=
          pipetune::DspBackendKind::simd ||
      reply.response.status.configuredDspSimdVariant !=
          pipetune::DspSimdVariant::automatic ||
      reply.response.status.effectiveDspBackend !=
          pipetune::DspBackendKind::simd ||
      reply.response.status.effectiveDspVariant !=
          pipetune::DspBackendVariant::simdBaseline ||
      reply.response.status.dspBackendFallback ||
      !reply.response.status.dspBackendError.empty()) {
    state.failed = true;
  } else {
    state.setDspBackendReply = true;
  }
  maybeStopServer(state);
}

static void maybeStartSetDspBackend(ClientTestState &state) {
  if (state.setRateReply && state.publishedSetRate &&
      !state.setDspBackendRequested) {
    state.setDspBackendRequested = true;
    pipetune_gtk::setControlDspBackendAsync(
        state.client, pipetune::DspBackendKind::simd,
        pipetune::DspSimdVariant::automatic,
        onSetDspBackendReply, &state);
  }
  maybeStopServer(state);
}

static void maybeStartSetRate(ClientTestState &state) {
  if (state.bypassReply && state.publishedBypass &&
      !state.setRateRequested) {
    state.setRateRequested = true;
    pipetune_gtk::setControlRateAsync(
        state.client,
        {.mode = pipetune::SampleRateMode::fixed,
         .fixedRate = 192000,
         .enforcement = pipetune::SampleRateEnforcement::force},
        onSetRateReply, &state);
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
  maybeStartSetRate(state);
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
             message.status.activePreset.empty()) {
    if (message.status.configuredRatePolicy.mode ==
            pipetune::SampleRateMode::fixed &&
        message.status.configuredRatePolicy.fixedRate == 192000 &&
        message.status.configuredRatePolicy.enforcement ==
            pipetune::SampleRateEnforcement::force) {
      if (message.status.configuredDspBackend ==
          pipetune::DspBackendKind::simd) {
        state.publishedSetDspBackend = true;
        maybeStopServer(state);
      } else {
        state.publishedSetRate = true;
        maybeStartSetDspBackend(state);
      }
      return;
    }
    if (!state.setRateRequested) {
      state.publishedBypass = true;
      maybeStartSetRate(state);
      return;
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
                  .ratePolicy = pipetune::defaultSampleRatePolicy(),
                  .dspBackend = pipetune::DspBackendKind::scalar,
                  .dspSimdVariant =
                      pipetune::DspSimdVariant::automatic};
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
      .setRateRequested = false,
      .setRateReply = false,
      .publishedSetRate = false,
      .setDspBackendRequested = false,
      .setDspBackendReply = false,
      .publishedSetDspBackend = false,
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
      !state.publishedBypass || !state.setRateReply ||
      !state.publishedSetRate || !state.setDspBackendReply ||
      !state.publishedSetDspBackend || !state.disconnected) {
    std::cerr << "asynchronous control client lifecycle differs\n";
    return 1;
  }
  return 0;
}
