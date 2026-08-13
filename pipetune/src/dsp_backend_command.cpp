/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_backend_command.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <sstream>
#include <string>
#include <utility>

namespace pipetune {

static DspBackendStatusQueryResult
dspBackendStatusError(std::string error) {
  return {.success = false,
          .status = {},
          .json = {},
          .error = std::move(error)};
}

DspBackendStatusQueryResult
queryDspBackendStatus(const std::filesystem::path &socketPath) {
  const auto exchange =
      exchangeControlMessage(socketPath, makeStatusControlRequest());
  if (!exchange.error.empty()) {
    return dspBackendStatusError(exchange.error);
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid || !response.success) {
    return dspBackendStatusError(std::move(response.error));
  }
  if (response.kind != ControlResponseKind::response) {
    return dspBackendStatusError(
        "daemon returned an unexpected DSP backend status event");
  }
  return {.success = true,
          .status = std::move(response.status),
          .json = exchange.response,
          .error = {}};
}

static PersistentDspBackendResult
dspBackendChangeError(std::string error) {
  return {.success = false,
          .liveApplied = false,
          .persistenceApplied = false,
          .status = {},
          .notice = {},
          .error = std::move(error)};
}

static std::string offlineBackendValidationError(
    DspBackendKind kind, DspSimdVariant simdVariant) {
  const auto backends = discoverDspBackends();
  const auto selected =
      selectDspBackend(kind, simdVariant, backends);
  if (selected.effectiveBackend == nullptr ||
      selected.effectiveBackend->kind() != kind) {
    return selected.error.empty()
               ? std::string(dspBackendName(kind)) +
                     " DSP backend is unavailable"
               : selected.error;
  }
  return {};
}

PersistentDspBackendResult
executeSetDspBackend(const PersistentDspBackendOptions &options,
                     DspBackendKind kind,
                     DspSimdVariant simdVariant) {
  if (kind != DspBackendKind::scalar && kind != DspBackendKind::simd) {
    return dspBackendChangeError("DSP backend is invalid");
  }
  if (dspSimdVariantName(simdVariant).empty() ||
      (kind == DspBackendKind::scalar &&
       simdVariant != DspSimdVariant::automatic)) {
    return dspBackendChangeError("DSP SIMD variant is invalid");
  }
  const auto request =
      makeSetDspBackendControlRequest(kind, simdVariant);
  if (request.empty()) {
    return dspBackendChangeError("cannot encode DSP backend request");
  }

  auto liveApplied = false;
  auto status = ControlRuntimeStatus{};
  auto notice = std::string{};
  const auto exchange =
      exchangeControlMessage(options.socketPath, request);
  if (!exchange.error.empty()) {
    const auto validation =
        offlineBackendValidationError(kind, simdVariant);
    if (!validation.empty()) {
      return dspBackendChangeError(
          "cannot select DSP backend while daemon is unavailable: " +
          validation);
    }
    notice =
        "running daemon is unavailable; the DSP backend will apply at next "
        "start: " +
        exchange.error;
  } else {
    auto response = parseControlResponse(exchange.response);
    if (!response.valid || !response.success) {
      return dspBackendChangeError(std::move(response.error));
    }
    if (response.kind != ControlResponseKind::response) {
      return dspBackendChangeError(
          "daemon returned an unexpected DSP backend status event");
    }
    const auto expectedPinned =
        concreteDspBackendVariant(simdVariant);
    const auto confirmed =
        response.status.configuredDspBackend == kind &&
        response.status.configuredDspSimdVariant == simdVariant &&
        response.status.effectiveDspBackend == kind &&
        (kind == DspBackendKind::scalar
             ? response.status.effectiveDspVariant ==
                   DspBackendVariant::scalar
             : (!expectedPinned.has_value() ||
                response.status.effectiveDspVariant ==
                    expectedPinned)) &&
        (simdVariant == DspSimdVariant::automatic ||
         (!response.status.dspBackendFallback &&
          response.status.dspBackendError.empty()));
    if (!confirmed) {
      return dspBackendChangeError(
          "daemon did not confirm the requested DSP backend");
    }
    liveApplied = true;
    status = std::move(response.status);
  }

  const auto persistenceError =
      saveDspBackendSelection(options.configPath, kind, simdVariant);
  if (!persistenceError.empty()) {
    const auto prefix =
        liveApplied
            ? std::string("DSP backend was applied live, but startup "
                          "persistence failed: ")
            : std::string("cannot persist DSP backend: ");
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

std::string formatDspBackendStatus(
    const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  formatted << "Configured backend: "
            << dspBackendName(status.configuredDspBackend) << '\n'
            << "Configured SIMD variant: "
            << dspSimdVariantName(status.configuredDspSimdVariant)
            << '\n'
            << "Effective backend: ";
  if (status.effectiveDspBackend.has_value()) {
    formatted << dspBackendName(*status.effectiveDspBackend);
  } else {
    formatted << "unavailable";
  }
  formatted << '\n' << "Effective variant: ";
  if (status.effectiveDspVariant.has_value()) {
    formatted << dspBackendVariantName(*status.effectiveDspVariant);
  } else {
    formatted << "unavailable";
  }
  formatted << '\n'
            << "Fallback: "
            << (status.dspBackendFallback ? "yes" : "no") << '\n';
  if (!status.dspBackendError.empty()) {
    formatted << "Backend error: " << status.dspBackendError << '\n';
  }
  return formatted.str();
}

std::string formatDspBackendList(
    const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  for (const auto &variant : status.availableDspVariants) {
    formatted << dspBackendVariantName(variant.variant) << ": "
              << (variant.available ? "available" : "unavailable")
              << " (CPU: " << variant.cpuRequirement << "; "
              << (variant.cpuSupported ? "supported" : "unsupported")
              << ')';
    if (!variant.error.empty()) {
      formatted << " - " << variant.error;
    }
    formatted << '\n';
  }
  return formatted.str();
}

} // namespace pipetune
