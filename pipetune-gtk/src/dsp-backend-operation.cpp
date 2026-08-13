/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp-backend-operation.h"

#include "pipetune/startup_config.h"

#include <string>

namespace pipetune_gtk {

static std::string validateBackend(
    pipetune::DspBackendKind kind,
    pipetune::DspSimdVariant simdVariant) {
  if (kind != pipetune::DspBackendKind::scalar &&
      kind != pipetune::DspBackendKind::simd) {
    return "DSP backend is invalid";
  }
  if (pipetune::dspSimdVariantName(simdVariant).empty() ||
      (kind == pipetune::DspBackendKind::scalar &&
       simdVariant != pipetune::DspSimdVariant::automatic)) {
    return "DSP SIMD variant is invalid";
  }
  const auto backends = pipetune::discoverDspBackends();
  const auto selected =
      pipetune::selectDspBackend(kind, simdVariant, backends);
  if (selected.effectiveBackend == nullptr ||
      selected.effectiveBackend->kind() != kind) {
    return selected.error.empty()
               ? std::string(pipetune::dspBackendName(kind)) +
                     " DSP backend is unavailable"
               : selected.error;
  }
  return {};
}

static std::string persistBackend(
    const DspBackendOperationRequest &request) {
  if (request.configPath.empty()) {
    return "startup configuration path is unavailable";
  }
  return pipetune::saveDspBackendSelection(
      request.configPath, request.kind, request.simdVariant);
}

static bool confirmsBackend(
    const pipetune::ControlRuntimeStatus &status,
    pipetune::DspBackendKind kind,
    pipetune::DspSimdVariant simdVariant) {
  const auto expectedPinned =
      pipetune::concreteDspBackendVariant(simdVariant);
  return status.configuredDspBackend == kind &&
         status.configuredDspSimdVariant == simdVariant &&
         status.effectiveDspBackend == kind &&
         (kind == pipetune::DspBackendKind::scalar
              ? status.effectiveDspVariant ==
                    pipetune::DspBackendVariant::scalar
              : (!expectedPinned.has_value() ||
                 status.effectiveDspVariant == expectedPinned)) &&
         (simdVariant == pipetune::DspSimdVariant::automatic ||
          (!status.dspBackendFallback &&
           status.dspBackendError.empty()));
}

DspBackendOperationCompletion
persistDspBackendOperationForNextStart(
    ApplicationState &state,
    const DspBackendOperationRequest &request) {
  const auto validationError =
      validateBackend(request.kind, request.simdVariant);
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
    const auto validationError =
        validateBackend(request.kind, request.simdVariant);
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
  if (!confirmsBackend(reply.response.status, request.kind,
                       request.simdVariant)) {
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
