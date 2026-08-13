/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "rate-operation.h"

#include "pipetune/startup_config.h"

#include <string>

namespace pipetune_gtk {

static std::string persistRatePolicy(
    const RateOperationRequest &request) {
  if (request.configPath.empty()) {
    return "startup configuration path is unavailable";
  }
  return pipetune::saveSampleRatePolicy(request.configPath,
                                        request.policy);
}

static bool confirmsRatePolicy(
    const pipetune::ControlRuntimeStatus &status,
    const pipetune::SampleRatePolicy &policy) {
  if (status.configuredRatePolicy != policy ||
      status.rateTransitioning ||
      status.dspSampleRate == 0) {
    return false;
  }
  if (policy.mode == pipetune::SampleRateMode::fixed) {
    return status.dspSampleRate == policy.fixedRate;
  }
  return status.graphSampleRate == 0 ||
         status.dspSampleRate == status.graphSampleRate;
}

RateOperationCompletion persistRateOperationForNextStart(
    ApplicationState &state, const RateOperationRequest &request) {
  const auto error = persistRatePolicy(request);
  if (!error.empty()) {
    setControlDiagnostic(
        state, "PCM rate policy could not be saved: " + error);
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }
  setControlDiagnostic(
      state, "PCM rate policy was saved for the next start");
  return {.reconnectRequired = false,
          .liveApplied = false,
          .persistenceApplied = true};
}

RateOperationCompletion completeRateOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const RateOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds) {
  setControlOperationPending(state, false);
  if (!reply.transportError.empty()) {
    markControlDisconnected(state, reply.transportError);
    const auto persistenceError = persistRatePolicy(request);
    if (persistenceError.empty()) {
      setControlDiagnostic(
          state,
          "Daemon disconnected; PCM rate policy was saved for the next "
          "start");
      return {.reconnectRequired = true,
              .liveApplied = false,
              .persistenceApplied = true};
    }
    setControlDiagnostic(
        state,
        "Daemon disconnected and PCM rate policy could not be saved: " +
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
  if (!confirmsRatePolicy(reply.response.status, request.policy)) {
    setControlDiagnostic(
        state, "Daemon did not confirm the requested PCM rate policy");
    return {.reconnectRequired = false,
            .liveApplied = false,
            .persistenceApplied = false};
  }

  const auto persistenceError = persistRatePolicy(request);
  if (!persistenceError.empty()) {
    setControlDiagnostic(
        state,
        "PCM rate policy was applied live, but startup persistence failed: " +
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
