#include "output-operation.h"

#include "pipetune/startup_config.h"

#include <string>

namespace pipetune_gtk {

OutputOperationCompletion completeOutputOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const OutputOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds) {
  setControlOperationPending(state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(state, reply.transportError);
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
  const auto preferenceConfirmed =
      request.clearPreference
          ? reply.response.status.preferredTarget.empty()
          : reply.response.status.preferredTarget == request.target;
  if (!preferenceConfirmed) {
    setControlDiagnostic(
        state, "Daemon did not confirm the requested output preference");
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }

  const auto persistenceError =
      request.clearPreference
          ? pipetune::clearPreferredOutput(request.configPath)
          : pipetune::savePreferredOutput(request.configPath,
                                          request.target);
  if (!persistenceError.empty()) {
    setControlDiagnostic(
        state,
        "Output preference was applied live, but startup persistence "
        "failed: " +
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
