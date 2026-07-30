#include "application-state.h"
#include "control-client.h"
#include "dsp-backend-operation.h"

#include "pipetune/control_protocol.h"
#include "pipetune/startup_config.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlRuntimeStatus backendStatus(
    pipetune::DspBackendKind configured,
    pipetune::DspBackendKind effective,
    pipetune::DspSimdVariant configuredVariant =
        pipetune::DspSimdVariant::automatic) {
  auto status = pipetune::ControlRuntimeStatus{};
  status.configuredDspBackend = configured;
  status.configuredDspSimdVariant = configuredVariant;
  status.effectiveDspBackend = effective;
  status.effectiveDspVariant =
      effective == pipetune::DspBackendKind::scalar
          ? std::optional{pipetune::DspBackendVariant::scalar}
          : pipetune::concreteDspBackendVariant(configuredVariant)
                .value_or(
                    pipetune::DspBackendVariant::simdBaseline);
  status.dspBackendFallback = configured != effective;
  status.dspBackendError =
      status.dspBackendFallback
          ? std::string("SIMD backend is unavailable")
          : std::string{};
  status.availableDspBackends = {{
      {.kind = pipetune::DspBackendKind::scalar,
       .available = true,
       .cpuRequirement = "none",
       .error = {}},
      {.kind = pipetune::DspBackendKind::simd,
       .available = !status.dspBackendFallback,
       .cpuRequirement = "test SIMD ISA",
       .error = status.dspBackendFallback
                    ? std::string("SIMD backend is unavailable")
                    : std::string{}},
  }};
  status.availableDspVariants = {
      {.variant = pipetune::DspBackendVariant::scalar,
       .available = true,
       .cpuSupported = true,
       .cpuRequirement = "none",
       .error = {}},
      {.variant = pipetune::DspBackendVariant::simdBaseline,
       .available = !status.dspBackendFallback,
       .cpuSupported = true,
       .cpuRequirement = "test SIMD ISA",
       .error = status.dspBackendFallback
                    ? std::string("SIMD backend is unavailable")
                    : std::string{}},
      {.variant = pipetune::DspBackendVariant::x86_64_v3,
       .available = !status.dspBackendFallback,
       .cpuSupported = true,
       .cpuRequirement = "x86-64-v3",
       .error = status.dspBackendFallback
                    ? std::string("SIMD backend is unavailable")
                    : std::string{}}};
  return status;
}

static pipetune_gtk::ApplicationState pendingState() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime = backendStatus(pipetune::DspBackendKind::scalar,
                                pipetune::DspBackendKind::scalar);
  state.operationPending = true;
  return state;
}

static pipetune_gtk::ControlClientReply successfulReply(
    pipetune::DspBackendKind kind,
    pipetune::DspSimdVariant variant =
        pipetune::DspSimdVariant::automatic) {
  return {
      .response = pipetune::parseControlResponse(
          pipetune::makeControlSuccessResponse(
              backendStatus(kind, kind, variant), {})),
      .transportError = {},
  };
}

static bool configHasBackendSelection(
    const std::filesystem::path &configPath,
    pipetune::DspBackendKind expectedKind,
    pipetune::DspSimdVariant expectedVariant =
        pipetune::DspSimdVariant::automatic) {
  const auto loaded = pipetune::loadStartupConfig(configPath);
  return check(loaded.error.empty(), loaded.error) &&
         check(loaded.dspBackend == expectedKind,
               "GTK stored DSP backend differs") &&
         check(loaded.dspSimdVariant == expectedVariant,
               "GTK stored DSP SIMD variant differs");
}

static bool testRejectedAndUnconfirmedReplies(
    const std::filesystem::path &configPath) {
  if (!check(pipetune::saveDspBackendKind(
                 configPath, pipetune::DspBackendKind::scalar)
                 .empty(),
             "cannot seed GTK DSP backend configuration")) {
    return false;
  }

  auto rejectedState = pendingState();
  const auto rejected = pipetune_gtk::completeDspBackendOperation(
      rejectedState,
      {.response = pipetune::parseControlResponse(
           pipetune::makeControlErrorResponse("backend rejected")),
       .transportError = {}},
      {.configPath = configPath,
       .kind = pipetune::DspBackendKind::simd},
      1000);
  if (!check(!rejected.liveApplied && !rejected.persistenceApplied &&
                 !rejected.reconnectRequired,
             "rejected GTK backend operation phases differ") ||
      !check(!rejectedState.operationPending,
             "rejected GTK backend operation must leave pending mode") ||
      !configHasBackendSelection(
          configPath, pipetune::DspBackendKind::scalar)) {
    return false;
  }

  auto unconfirmedState = pendingState();
  const auto fallback = pipetune_gtk::ControlClientReply{
      .response = pipetune::parseControlResponse(
          pipetune::makeControlSuccessResponse(
              backendStatus(pipetune::DspBackendKind::simd,
                            pipetune::DspBackendKind::scalar),
              {})),
      .transportError = {},
  };
  const auto unconfirmed = pipetune_gtk::completeDspBackendOperation(
      unconfirmedState, fallback,
      {.configPath = configPath,
       .kind = pipetune::DspBackendKind::simd},
      2000);
  return check(!unconfirmed.liveApplied &&
                   !unconfirmed.persistenceApplied,
               "fallback reply must not confirm SIMD selection") &&
         check(unconfirmedState.diagnostic.find("did not confirm") !=
                   std::string::npos,
               "unconfirmed backend must explain the failure") &&
         configHasBackendSelection(
             configPath, pipetune::DspBackendKind::scalar);
}

static bool testSuccessfulAndOfflineChanges(
    const std::filesystem::path &configPath) {
  auto state = pendingState();
  const auto changed = pipetune_gtk::completeDspBackendOperation(
      state,
      successfulReply(pipetune::DspBackendKind::simd,
                      pipetune::DspSimdVariant::x86_64_v3),
      {.configPath = configPath,
       .kind = pipetune::DspBackendKind::simd,
       .simdVariant = pipetune::DspSimdVariant::x86_64_v3},
      3000);
  if (!check(changed.liveApplied && changed.persistenceApplied &&
                 !changed.reconnectRequired,
             "successful GTK backend operation phases differ") ||
      !check(state.runtime.configuredDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 state.runtime.effectiveDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 !state.operationPending,
             "successful GTK backend operation state differs") ||
      !configHasBackendSelection(
          configPath, pipetune::DspBackendKind::simd,
          pipetune::DspSimdVariant::x86_64_v3)) {
    return false;
  }

  auto offlineState = pipetune_gtk::initialApplicationState();
  const auto offline =
      pipetune_gtk::persistDspBackendOperationForNextStart(
          offlineState,
          {.configPath = configPath,
           .kind = pipetune::DspBackendKind::scalar});
  return check(!offline.liveApplied && offline.persistenceApplied &&
                   !offline.reconnectRequired,
               "offline GTK backend persistence phases differ") &&
         check(offlineState.diagnostic.find("next start") !=
                   std::string::npos,
               "offline GTK backend persistence must be explicit") &&
         configHasBackendSelection(
             configPath, pipetune::DspBackendKind::scalar);
}

static bool testDisconnectPersistsForNextStart(
    const std::filesystem::path &configPath) {
  auto state = pendingState();
  const auto changed = pipetune_gtk::completeDspBackendOperation(
      state, {.response = {}, .transportError = "daemon disconnected"},
      {.configPath = configPath,
       .kind = pipetune::DspBackendKind::scalar},
      4000);
  return check(changed.reconnectRequired && !changed.liveApplied &&
                   changed.persistenceApplied,
               "disconnected GTK backend operation phases differ") &&
         check(state.connection ==
                   pipetune_gtk::ControlConnectionState::disconnected,
               "backend transport failure must disconnect GTK state") &&
         check(state.diagnostic.find("next start") != std::string::npos,
               "backend transport failure must report offline persistence") &&
         configHasBackendSelection(
             configPath, pipetune::DspBackendKind::scalar);
}

static pipetune::DspSimdVariant unavailableForeignVariant() {
#if defined(__aarch64__)
  return pipetune::DspSimdVariant::x86_64_v4;
#else
  return pipetune::DspSimdVariant::arm64Sve;
#endif
}

static bool testUnavailablePinDoesNotPersist(
    const std::filesystem::path &configPath) {
  const auto seeded = pipetune::saveDspBackendSelection(
      configPath, pipetune::DspBackendKind::scalar,
      pipetune::DspSimdVariant::automatic);
  auto state = pipetune_gtk::initialApplicationState();
  const auto changed =
      pipetune_gtk::persistDspBackendOperationForNextStart(
          state,
          {.configPath = configPath,
           .kind = pipetune::DspBackendKind::simd,
           .simdVariant = unavailableForeignVariant()});
  return check(seeded.empty(), seeded) &&
         check(!changed.liveApplied && !changed.persistenceApplied &&
                   !changed.reconnectRequired,
               "unavailable pinned GTK backend must not persist") &&
         check(state.diagnostic.find("could not be selected") !=
                   std::string::npos,
               "unavailable pinned GTK backend must explain rejection") &&
         configHasBackendSelection(
             configPath, pipetune::DspBackendKind::scalar);
}

static bool testPersistenceFailure(
    const std::filesystem::path &directory) {
  const auto blockedParent = directory / "not-a-directory";
  {
    auto stream = std::ofstream(blockedParent, std::ios::binary);
    stream << "blocking file";
  }
  auto state = pendingState();
  const auto changed = pipetune_gtk::completeDspBackendOperation(
      state, successfulReply(pipetune::DspBackendKind::scalar),
      {.configPath = blockedParent / "environment",
       .kind = pipetune::DspBackendKind::scalar},
      5000);
  return check(changed.liveApplied && !changed.persistenceApplied,
               "GTK partial backend success phases differ") &&
         check(state.diagnostic.find("applied live") != std::string::npos,
               "GTK partial backend success must be explicit");
}

int main() {
  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-gtk-dsp-backend-operation-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto configPath = directory / "config" / "environment";
  const auto passed =
      testRejectedAndUnconfirmedReplies(configPath) &&
      testSuccessfulAndOfflineChanges(configPath) &&
      testDisconnectPersistsForNextStart(configPath) &&
      testUnavailablePinDoesNotPersist(configPath) &&
      testPersistenceFailure(directory);
  std::filesystem::remove_all(directory);
  return passed ? 0 : 1;
}
