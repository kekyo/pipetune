#include "rate_command.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune {

static RateStatusQueryResult rateStatusError(std::string error) {
  return {.success = false,
          .status = {},
          .json = {},
          .error = std::move(error)};
}

RateStatusQueryResult
queryRateStatus(const std::filesystem::path &socketPath) {
  const auto exchange =
      exchangeControlMessage(socketPath, makeStatusControlRequest());
  if (!exchange.error.empty()) {
    return rateStatusError(exchange.error);
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid || !response.success) {
    return rateStatusError(std::move(response.error));
  }
  if (response.kind != ControlResponseKind::response) {
    return rateStatusError("daemon returned an unexpected status event");
  }
  return {.success = true,
          .status = std::move(response.status),
          .json = exchange.response,
          .error = {}};
}

static PersistentRateResult rateChangeError(std::string error) {
  return {.success = false,
          .liveApplied = false,
          .persistenceApplied = false,
          .status = {},
          .notice = {},
          .error = std::move(error)};
}

PersistentRateResult
executeSetSampleRatePolicy(const PersistentRateOptions &options,
                           const SampleRatePolicy &policy) {
  if (!sampleRatePolicyIsValid(policy)) {
    return rateChangeError("sample-rate policy is invalid");
  }
  const auto request = makeSetRateControlRequest(policy);
  if (request.empty()) {
    return rateChangeError("cannot encode sample-rate request");
  }

  auto liveApplied = false;
  auto status = ControlRuntimeStatus{};
  auto notice = std::string{};
  const auto exchange =
      exchangeControlMessage(options.socketPath, request);
  if (!exchange.error.empty()) {
    notice =
        "running daemon is unavailable; the rate will apply at next start: " +
        exchange.error;
  } else {
    auto response = parseControlResponse(exchange.response);
    if (!response.valid || !response.success) {
      return rateChangeError(std::move(response.error));
    }
    if (response.kind != ControlResponseKind::response) {
      return rateChangeError(
          "daemon returned an unexpected rate status event");
    }
    const auto fixedDspConfirmed =
        policy.mode != SampleRateMode::fixed ||
        response.status.dspSampleRate == policy.fixedRate;
    const auto automaticDspConfirmed =
        policy.mode != SampleRateMode::automatic ||
        response.status.graphSampleRate == 0 ||
        response.status.dspSampleRate == response.status.graphSampleRate;
    if (response.status.configuredRatePolicy != policy ||
        response.status.rateTransitioning ||
        response.status.dspSampleRate == 0 || !fixedDspConfirmed ||
        !automaticDspConfirmed) {
      return rateChangeError(
          "daemon did not confirm the requested sample-rate policy");
    }
    liveApplied = true;
    status = std::move(response.status);
  }

  const auto persistenceError =
      saveSampleRatePolicy(options.configPath, policy);
  if (!persistenceError.empty()) {
    const auto prefix =
        liveApplied
            ? std::string("sample-rate policy was applied live, but startup "
                          "persistence failed: ")
            : std::string("cannot persist sample-rate policy: ");
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

static std::string formatRate(std::uint32_t rate) {
  if (rate == 0) {
    return "inactive";
  }
  auto formatted = std::ostringstream{};
  if (rate % 1000 == 0) {
    formatted << rate / 1000;
  } else {
    formatted << std::fixed << std::setprecision(1)
              << static_cast<double>(rate) / 1000.0;
  }
  formatted << " kHz";
  return formatted.str();
}

std::string formatSampleRateStatus(
    const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  formatted << "Configured rate: ";
  if (status.configuredRatePolicy.mode == SampleRateMode::automatic) {
    formatted << "Automatic";
  } else {
    formatted << formatRate(status.configuredRatePolicy.fixedRate)
              << " ("
              << sampleRateEnforcementName(
                     status.configuredRatePolicy.enforcement)
              << ')';
  }
  formatted << '\n'
            << "DSP rate: " << formatRate(status.dspSampleRate) << '\n'
            << "PipeWire graph rate: "
            << formatRate(status.graphSampleRate) << '\n'
            << "Transition: "
            << (status.rateTransitioning ? "in progress" : "idle") << '\n';
  if (!status.rateError.empty()) {
    formatted << "Last rate error: " << status.rateError << '\n';
  }
  return formatted.str();
}

std::string formatSelectableSampleRates() {
  auto formatted = std::ostringstream{};
  formatted << "automatic: follow the PipeWire graph\n";
  for (const auto rate : selectableSampleRates()) {
    formatted << formatRate(rate) << ": fixed\n";
  }
  return formatted.str();
}

} // namespace pipetune
