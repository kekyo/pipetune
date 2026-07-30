#include "dsp-backend-operation.h"

#include "pipetune/startup_config.h"

#include <string>

namespace pipetune_gtk {

static std::string validateBackend(
    pipetune::DspBackendKind kind) {
  if (kind != pipetune::DspBackendKind::scalar &&
      kind != pipetune::DspBackendKind::simd) {
    return "DSP backend is invalid";
  }
  const auto backends = pipetune::discoverDspBackends();
  if (backends.scalar.backend == nullptr) {
    return backends.scalar.error.empty()
               ? std::string("scalar DSP backend is unavailable")
               : backends.scalar.error;
  }
  const auto &requested = backends.get(kind);
  if (requested.backend == nullptr) {
    return requested.error.empty()
               ? std::string(pipetune::dspBackendName(kind)) +
                     " DSP backend is unavailable"
               : requested.error;
  }
  return {};
}

static std::string persistBackend(
    const DspBackendOperationRequest &request) {
  if (request.configPath.empty()) {
    return "startup configuration path is unavailable";
  }
  return pipetune::saveDspBackendKind(request.configPath,
                                      request.kind);
}

static bool confirmsBackend(
    const pipetune::ControlRuntimeStatus &status,
    pipetune::DspBackendKind kind) {
  return status.configuredDspBackend == kind &&
         status.effectiveDspBackend == kind &&
         !status.dspBackendFallback &&
         status.dspBackendError.empty();
}

DspBackendOperationCompletion
persistDspBackendOperationForNextStart(
    ApplicationState &state,
    const DspBackendOperationRequest &request) {
  const auto validationError = validateBackend(request.kind);
  if (!validationError.empty()) {
    setControlDiagnostic(
        state, "DSP backend could not be selected: " +
                   validationError);
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }
  const auto persistenceError = persistBackend(request);
  if (!persistenceError.empty()) {
    setControlDiagnostic(
        state, "DSP backend could not be saved: " +
                   persistenceError);
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }
  setControlDiagnostic(
      state, "DSP backend was saved for the next start");
  return {.reconnectRequired = false,
          .liveApplied = false,
          .persistenceApplied = true};
}

DspBackendOperationCompletion completeDspBackendOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const DspBackendOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds) {
  setControlOperationPending(state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(state, reply.transportError);
    const auto validationError = validateBackend(request.kind);
    if (!validationError.empty()) {
      setControlDiagnostic(
          state,
          "Daemon disconnected and DSP backend could not be selected: " +
              validationError);
      return {.reconnectRequired = true,
              .liveApplied = false,
              .persistenceApplied = false};
    }
    const auto persistenceError = persistBackend(request);
    if (!persistenceError.empty()) {
      setControlDiagnostic(
          state,
          "Daemon disconnected and DSP backend could not be saved: " +
              persistenceError);
      return {.reconnectRequired = true,
              .liveApplied = false,
              .persistenceApplied = false};
    }
    setControlDiagnostic(
        state,
        "Daemon disconnected; DSP backend was saved for the next start");
    return {.reconnectRequired = true,
            .liveApplied = false,
            .persistenceApplied = true};
  }

  applyControlResponse(state, reply.response,
                       receivedAtMonotonicMilliseconds);
  if (!reply.response.valid || !reply.response.success) {
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }
  if (!confirmsBackend(reply.response.status, request.kind)) {
    setControlDiagnostic(
        state, "Daemon did not confirm the requested DSP backend");
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }

  const auto persistenceError = persistBackend(request);
  if (!persistenceError.empty()) {
    setControlDiagnostic(
        state,
        "DSP backend was applied live, but startup persistence failed: " +
            persistenceError);
    return {.reconnectRequired = false,
            .liveApplied = true,
            .persistenceApplied = false};
  }
  return {.reconnectRequired = false,
          .liveApplied = true,
          .persistenceApplied = true};
}

} // namespace pipetune_gtk
