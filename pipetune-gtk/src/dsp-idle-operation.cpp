#include "dsp-idle-operation.h"

#include "pipetune/startup_config.h"

#include <string>

namespace pipetune_gtk {

static std::string persistDspIdlePolicy(
    const DspIdleOperationRequest &request) {
  if (request.configPath.empty()) {
    return "startup configuration path is unavailable";
  }
  return pipetune::saveDspIdlePolicy(request.configPath,
                                     request.policy);
}

DspIdleOperationCompletion persistDspIdleOperationForNextStart(
    ApplicationState &state, const DspIdleOperationRequest &request) {
  const auto error = persistDspIdlePolicy(request);
  if (!error.empty()) {
    setControlDiagnostic(
        state, "DSP idle policy could not be saved: " + error);
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }
  setControlDiagnostic(
      state, "DSP idle policy was saved for the next start");
  return {.reconnectRequired = false,
          .liveApplied = false,
          .persistenceApplied = true};
}

DspIdleOperationCompletion completeDspIdleOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const DspIdleOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds) {
  setControlOperationPending(state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(state, reply.transportError);
    const auto persistenceError = persistDspIdlePolicy(request);
    if (persistenceError.empty()) {
      setControlDiagnostic(
          state,
          "Daemon disconnected; DSP idle policy was saved for the next "
          "start");
      return {.reconnectRequired = true,
              .liveApplied = false,
              .persistenceApplied = true};
    }
    setControlDiagnostic(
        state,
        "Daemon disconnected and DSP idle policy could not be saved: " +
            persistenceError);
    return {.reconnectRequired = true,
            .liveApplied = false,
            .persistenceApplied = false};
  }

  applyControlResponse(state, reply.response,
                       receivedAtMonotonicMilliseconds);
  if (!reply.response.valid || !reply.response.success) {
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }
  if (reply.response.status.dspIdlePolicy != request.policy) {
    setControlDiagnostic(
        state, "Daemon did not confirm the requested DSP idle policy");
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }

  const auto persistenceError = persistDspIdlePolicy(request);
  if (!persistenceError.empty()) {
    setControlDiagnostic(
        state,
        "DSP idle policy was applied live, but startup persistence failed: " +
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
