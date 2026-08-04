#include "rate_command.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <algorithm>
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

static bool outputConfirmsRatePolicy(
    const ControlFilterOutputStatus &output,
    const SampleRatePolicy &policy) {
  if (output.state == ControlFilterState::bypassed ||
      output.state == ControlFilterState::error) {
    return true;
  }
  return output.dspSampleRate != 0 && output.outputSampleRate != 0 &&
         (policy.mode != SampleRateMode::fixed ||
          output.dspSampleRate == policy.fixedRate);
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
    const auto confirmed =
        response.status.configuredRatePolicy == policy &&
        std::all_of(response.status.filterOutputs.begin(),
                    response.status.filterOutputs.end(),
                    [&policy](const auto &output) {
                      return outputConfirmsRatePolicy(output, policy);
                    });
    if (!confirmed) {
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

static std::string_view filterStateText(ControlFilterState state) {
  switch (state) {
  case ControlFilterState::waiting:
    return "waiting";
  case ControlFilterState::active:
    return "active";
  case ControlFilterState::bypassed:
    return "direct route";
  case ControlFilterState::error:
    return "error";
  }
  return "unknown";
}

std::string formatSampleRateStatus(
    const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  formatted << "Configured rate: ";
  if (status.configuredRatePolicy.mode == SampleRateMode::maximum) {
    formatted << "Max";
  } else {
    formatted << formatRate(status.configuredRatePolicy.fixedRate);
  }
  formatted << " ("
            << sampleRateEnforcementName(
                   status.configuredRatePolicy.enforcement)
            << ")\n";
  if (status.filterOutputs.empty()) {
    formatted << "No physical outputs are currently available.\n";
    return formatted.str();
  }
  for (const auto &output : status.filterOutputs) {
    const auto &label = output.targetDescription.empty()
                            ? output.targetNodeName
                            : output.targetDescription;
    formatted << label << " (" << output.targetNodeName << ")\n"
              << "  Filter: " << filterStateText(output.state) << '\n'
              << "  DSP rate: " << formatRate(output.dspSampleRate) << '\n'
              << "  PipeWire request: "
              << formatRate(output.outputSampleRate) << '\n'
              << "  Active physical rate: "
              << formatRate(output.activeOutputSampleRate) << '\n'
              << "  Resampling fallback: "
              << (output.rateFallback ? "yes" : "no") << '\n';
    if (!output.error.empty()) {
      formatted << "  Diagnostic: " << output.error << '\n';
    }
  }
  return formatted.str();
}

std::string formatSampleRateCapabilities(
    const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  if (status.filterOutputs.empty()) {
    formatted << "No audio output devices are currently available.\n";
    return formatted.str();
  }
  for (const auto &output : status.filterOutputs) {
    const auto &label = output.targetDescription.empty()
                            ? output.targetNodeName
                            : output.targetDescription;
    formatted << label << " (" << output.targetNodeName << ")\n";
    for (const auto rate : selectableSampleRates()) {
      formatted << "  " << formatRate(rate) << ": ";
      if (!output.sampleRateCapabilities.known) {
        formatted << "unknown";
      } else if (sampleRateCapabilitiesSupport(
                     output.sampleRateCapabilities, rate)) {
        formatted << "supported";
      } else {
        formatted << "unsupported";
      }
      formatted << '\n';
    }
  }
  return formatted.str();
}

} // namespace pipetune
