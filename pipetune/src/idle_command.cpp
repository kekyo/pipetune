#include "idle_command.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

namespace pipetune {

static IdleStatusQueryResult idleStatusError(std::string error) {
  return {.success = false,
          .status = {},
          .json = {},
          .error = std::move(error)};
}

IdleStatusQueryResult
queryIdleStatus(const std::filesystem::path &socketPath) {
  const auto exchange =
      exchangeControlMessage(socketPath, makeStatusControlRequest());
  if (!exchange.error.empty()) {
    return idleStatusError(exchange.error);
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid || !response.success) {
    return idleStatusError(std::move(response.error));
  }
  if (response.kind != ControlResponseKind::response) {
    return idleStatusError("daemon returned an unexpected status event");
  }
  return {.success = true,
          .status = std::move(response.status),
          .json = exchange.response,
          .error = {}};
}

static PersistentIdleResult idleChangeError(std::string error) {
  return {.success = false,
          .liveApplied = false,
          .persistenceApplied = false,
          .status = {},
          .notice = {},
          .error = std::move(error)};
}

PersistentIdleResult
executeSetDspIdlePolicy(const PersistentIdleOptions &options,
                        DspIdlePolicy policy) {
  const auto request = makeSetDspIdlePolicyControlRequest(policy);
  if (request.empty()) {
    return idleChangeError("DSP idle policy is invalid");
  }

  auto liveApplied = false;
  auto status = ControlRuntimeStatus{};
  auto notice = std::string{};
  const auto exchange =
      exchangeControlMessage(options.socketPath, request);
  if (!exchange.error.empty()) {
    notice =
        "running daemon is unavailable; the DSP idle policy will apply at "
        "next start: " +
        exchange.error;
  } else {
    auto response = parseControlResponse(exchange.response);
    if (!response.valid || !response.success) {
      return idleChangeError(std::move(response.error));
    }
    if (response.kind != ControlResponseKind::response) {
      return idleChangeError(
          "daemon returned an unexpected DSP idle status event");
    }
    if (response.status.dspIdlePolicy != policy) {
      return idleChangeError(
          "daemon did not confirm the requested DSP idle policy");
    }
    liveApplied = true;
    status = std::move(response.status);
  }

  const auto persistenceError =
      saveDspIdlePolicy(options.configPath, policy);
  if (!persistenceError.empty()) {
    const auto prefix =
        liveApplied
            ? std::string("DSP idle policy was applied live, but startup "
                          "persistence failed: ")
            : std::string("cannot persist DSP idle policy: ");
    return {.success = false,
            .liveApplied = liveApplied,
            .persistenceApplied = false,
            .status = std::move(status),
            .notice = std::move(notice),
            .error = prefix + persistenceError};
  }
  return {.success = true,
          .liveApplied = liveApplied,
          .persistenceApplied = true,
          .status = std::move(status),
          .notice = std::move(notice),
          .error = {}};
}

static std::string formatCounter(std::uint64_t value) {
  auto digits = std::to_string(value);
  for (auto position =
           static_cast<std::ptrdiff_t>(digits.size()) - 3;
       position > 0; position -= 3) {
    digits.insert(static_cast<std::size_t>(position), 1, ',');
  }
  return digits;
}

std::string formatIdleStatus(const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  formatted << "DSP idle policy: "
            << dspIdlePolicyName(status.dspIdlePolicy) << '\n'
            << "DSP idle state: "
            << dspIdleStateName(status.dspIdleState) << '\n'
            << "Skipped frames: "
            << formatCounter(status.dspIdleSkippedFrames) << '\n'
            << "Sleep transitions: "
            << formatCounter(status.dspIdleSleepTransitions) << '\n'
            << "PipeWire graph: "
            << (status.pipeWireIdle ? "paused" : "running") << '\n';
  return formatted.str();
}

} // namespace pipetune
