#include "control-client.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"

#include <gio/gio.h>

#include <algorithm>
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
  const auto dspRate =
      state.ratePolicy.mode == pipetune::SampleRateMode::maximum
          ? 96000u
          : state.ratePolicy.fixedRate;
  const auto outputRate = std::min(dspRate, 96000u);
  return {
      .processingMode = state.bypassed
                            ? pipetune::ProcessingMode::bypass
                            : pipetune::ProcessingMode::preset,
      .activePreset = state.bypassed ? std::string{} : state.activePreset,
      .configurationError = {},
      .activePluginCount = state.bypassed ? 0u : 3u,
      .policyBackend = "wireplumber-0.5",
      .filterOutputs =
          {{.targetNodeName = "alsa_output.test",
            .targetDescription = "Test Output",
            .filterNodeName = "pipetune.filter.test",
            .state = pipetune::ControlFilterState::active,
            .error = {},
            .channelCount = 2,
            .sampleRateCapabilities =
                {.known = true,
                 .constraints =
                     {{.kind = pipetune::SampleRateConstraintKind::range,
                       .minimum = 44100,
                       .maximum = 96000,
                       .step = 0}}},
            .dspSampleRate = dspRate,
            .outputSampleRate = outputRate,
            .activeOutputSampleRate = outputRate,
            .rateFallback = outputRate != dspRate,
            .latencyFrames = 64,
            .overrunFrames = 0,
            .underrunFrames = 0,
            .processingErrors = 0,
            .dspProcessedFrames = 0,
            .dspProcessingNanoseconds = 0}},
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
      .configuredRatePolicy = state.ratePolicy,
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
           {.variant = pipetune::DspBackendVariant::simdBaseline,
            .available = true,
            .cpuSupported = true,
            .cpuRequirement = "test SIMD ISA",
            .error = {}}},
  };
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
  {
    auto lock = std::scoped_lock(state.mutex);
    switch (request.request.command) {
    case pipetune::ControlCommand::loadPreset:
      state.activePreset = request.request.presetPath.string();
      state.bypassed = false;
      break;
    case pipetune::ControlCommand::bypass:
      state.activePreset.clear();
      state.bypassed = true;
      break;
    case pipetune::ControlCommand::setRate:
      state.ratePolicy = request.request.ratePolicy;
      break;
    case pipetune::ControlCommand::setDspBackend:
      state.dspBackend = request.request.dspBackend;
      state.dspSimdVariant = request.request.dspSimdVariant;
      break;
    case pipetune::ControlCommand::status:
    case pipetune::ControlCommand::subscribe:
      break;
    }
  }
  return {.response =
              pipetune::makeControlSuccessResponse(serverStatus(state), {}),
          .connectionMode = pipetune::ControlConnectionMode::close,
          .publishStatus = request.request.command !=
                           pipetune::ControlCommand::status};
}

struct ClientTestState {
  GMainLoop *loop;
  pipetune_gtk::ControlClient *client;
  std::unique_ptr<pipetune::ControlServer> *server;
  bool connected;
  bool initialStatus;
  bool loadRequested;
  bool loadReply;
  bool publishedLoad;
  bool bypassRequested;
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
  if (state.loadReply && state.publishedLoad && state.bypassReply &&
      state.publishedBypass && state.setRateReply &&
      state.publishedSetRate && state.setDspBackendReply &&
      state.publishedSetDspBackend && *state.server != nullptr) {
    g_idle_add(stopServer, &state);
  }
}

static void maybeStartBypass(ClientTestState &state);
static void maybeStartSetRate(ClientTestState &state);
static void maybeStartSetDspBackend(ClientTestState &state);

static void onSetDspBackendReply(
    const pipetune_gtk::ControlClientReply &reply, void *userData) {
  auto &state = *static_cast<ClientTestState *>(userData);
  if (!reply.transportError.empty() || !reply.response.valid ||
      !reply.response.success ||
      reply.response.status.configuredDspBackend !=
          pipetune::DspBackendKind::simd ||
      reply.response.status.effectiveDspVariant !=
          pipetune::DspBackendVariant::simdBaseline) {
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
        pipetune::DspSimdVariant::automatic, onSetDspBackendReply, &state);
  }
  maybeStopServer(state);
}

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
      reply.response.status.filterOutputs.size() != 1 ||
      reply.response.status.filterOutputs[0].dspSampleRate != 192000 ||
      reply.response.status.filterOutputs[0].outputSampleRate != 96000 ||
      !reply.response.status.filterOutputs[0].rateFallback) {
    state.failed = true;
  } else {
    state.setRateReply = true;
  }
  maybeStartSetDspBackend(state);
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

static void maybeStartBypass(ClientTestState &state) {
  if (state.loadReply && state.publishedLoad && !state.bypassRequested) {
    state.bypassRequested = true;
    pipetune_gtk::bypassControlAsync(state.client, onBypassReply, &state);
  }
  maybeStopServer(state);
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
  maybeStartBypass(state);
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
    state.initialStatus = true;
    if (!state.loadRequested) {
      state.loadRequested = true;
      pipetune_gtk::loadControlPresetAsync(
          state.client, "/tmp/selected.effetune_preset", onLoadReply,
          &state);
    }
  } else if (message.status.activePreset ==
             "/tmp/selected.effetune_preset") {
    state.publishedLoad = true;
    maybeStartBypass(state);
  } else if (message.status.processingMode ==
                 pipetune::ProcessingMode::bypass &&
             message.status.configuredRatePolicy.mode ==
                 pipetune::SampleRateMode::maximum) {
    state.publishedBypass = true;
    maybeStartSetRate(state);
  } else if (message.status.processingMode ==
                 pipetune::ProcessingMode::bypass &&
             message.status.configuredRatePolicy.mode ==
                 pipetune::SampleRateMode::fixed &&
             message.status.configuredDspBackend ==
                 pipetune::DspBackendKind::scalar) {
    state.publishedSetRate = true;
    maybeStartSetDspBackend(state);
  } else if (message.status.processingMode ==
                 pipetune::ProcessingMode::bypass &&
             message.status.configuredDspBackend ==
                 pipetune::DspBackendKind::simd) {
    state.publishedSetDspBackend = true;
    maybeStopServer(state);
  } else {
    state.failed = true;
  }
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
      .loadRequested = false,
      .loadReply = false,
      .publishedLoad = false,
      .bypassRequested = false,
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
      !state.initialStatus || !state.loadReply || !state.publishedLoad ||
      !state.bypassReply || !state.publishedBypass ||
      !state.setRateReply || !state.publishedSetRate ||
      !state.setDspBackendReply || !state.publishedSetDspBackend ||
      !state.disconnected) {
    std::cerr << "asynchronous control client lifecycle differs\n";
    return 1;
  }
  return 0;
}
