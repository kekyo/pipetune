#include "settings-transaction.h"

#include <string>

namespace pipetune_gtk {

static bool configMatches(const pipetune::StartupConfig &left,
                          const pipetune::StartupConfig &right) {
  return left.presetFound == right.presetFound &&
         left.presetPath == right.presetPath &&
         left.preferredOutputFound == right.preferredOutputFound &&
         left.preferredOutput == right.preferredOutput &&
         left.ratePolicy == right.ratePolicy &&
         left.dspBackend == right.dspBackend &&
         left.dspSimdVariant == right.dspSimdVariant;
}

static bool operationMatches(
    SettingsOperation operation,
    const pipetune::StartupConfig &left,
    const pipetune::StartupConfig &right) {
  switch (operation) {
  case SettingsOperation::none:
    return true;
  case SettingsOperation::output:
    return left.preferredOutputFound == right.preferredOutputFound &&
           left.preferredOutput == right.preferredOutput;
  case SettingsOperation::rate:
    return left.ratePolicy == right.ratePolicy;
  case SettingsOperation::dspBackend:
    return left.dspBackend == right.dspBackend &&
           left.dspSimdVariant == right.dspSimdVariant;
  case SettingsOperation::processing:
    return left.presetFound == right.presetFound &&
           left.presetPath == right.presetPath;
  }
  return false;
}

static std::string confirmationDiagnostic(SettingsOperation operation) {
  switch (operation) {
  case SettingsOperation::output:
    return "Daemon did not confirm the requested output";
  case SettingsOperation::rate:
    return "Daemon did not confirm the requested sample-rate policy";
  case SettingsOperation::dspBackend:
    return "Daemon did not confirm the requested DSP backend";
  case SettingsOperation::processing:
    return "Daemon did not confirm the requested processing mode";
  case SettingsOperation::none:
    return "No settings operation was in flight";
  }
  return "Daemon did not confirm the requested setting";
}

SettingsTransaction beginSettingsTransaction(
    const pipetune::StartupConfig &saved,
    const pipetune::StartupConfig &live, bool connected) {
  return {
      .saved = saved,
      .baselineLive = live,
      .desiredLive = live,
      .confirmedLive = live,
      .inFlight = SettingsOperation::none,
      .inFlightTarget = live,
      .connected = connected,
      .cancelRequested = false,
      .conflict = false,
      .liveChangeFailed = false,
      .diagnostic = {},
  };
}

void editSettingsTransaction(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &desired) {
  if (transaction.cancelRequested) {
    return;
  }
  transaction.desiredLive = desired;
  transaction.liveChangeFailed = false;
  if (!transaction.conflict) {
    transaction.diagnostic.clear();
  }
}

SettingsOperation
nextSettingsOperation(const SettingsTransaction &transaction) {
  if (!transaction.connected ||
      transaction.inFlight != SettingsOperation::none ||
      transaction.conflict || transaction.liveChangeFailed) {
    return SettingsOperation::none;
  }
  if (!operationMatches(SettingsOperation::output,
                        transaction.confirmedLive,
                        transaction.desiredLive)) {
    return SettingsOperation::output;
  }
  if (!operationMatches(SettingsOperation::rate,
                        transaction.confirmedLive,
                        transaction.desiredLive)) {
    return SettingsOperation::rate;
  }
  if (!operationMatches(SettingsOperation::dspBackend,
                        transaction.confirmedLive,
                        transaction.desiredLive)) {
    return SettingsOperation::dspBackend;
  }
  if (!operationMatches(SettingsOperation::processing,
                        transaction.confirmedLive,
                        transaction.desiredLive)) {
    return SettingsOperation::processing;
  }
  return SettingsOperation::none;
}

bool beginSettingsOperation(SettingsTransaction &transaction,
                            SettingsOperation operation) {
  if (operation == SettingsOperation::none ||
      nextSettingsOperation(transaction) != operation) {
    return false;
  }
  transaction.inFlight = operation;
  transaction.inFlightTarget = transaction.desiredLive;
  return true;
}

void completeSettingsOperation(
    SettingsTransaction &transaction, bool success,
    const pipetune::StartupConfig &confirmed,
    std::string_view diagnostic) {
  const auto operation = transaction.inFlight;
  if (operation == SettingsOperation::none) {
    return;
  }
  transaction.inFlight = SettingsOperation::none;
  if (!success) {
    transaction.liveChangeFailed = true;
    transaction.diagnostic =
        diagnostic.empty() ? confirmationDiagnostic(operation)
                           : std::string(diagnostic);
    return;
  }
  if (!operationMatches(operation, confirmed,
                        transaction.inFlightTarget)) {
    transaction.liveChangeFailed = true;
    transaction.diagnostic = confirmationDiagnostic(operation);
    return;
  }
  transaction.confirmedLive = confirmed;
  transaction.liveChangeFailed = false;
  transaction.diagnostic.clear();
}

void markSettingsDisconnected(SettingsTransaction &transaction,
                              std::string_view diagnostic) {
  transaction.connected = false;
  transaction.inFlight = SettingsOperation::none;
  transaction.liveChangeFailed = false;
  transaction.diagnostic = diagnostic;
}

void reconnectSettingsTransaction(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &live) {
  transaction.connected = true;
  transaction.confirmedLive = live;
  transaction.inFlight = SettingsOperation::none;
  transaction.inFlightTarget = live;
  transaction.conflict = false;
  transaction.liveChangeFailed = false;
  transaction.diagnostic.clear();
}

void observeSettingsRuntime(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &live) {
  if (!transaction.connected) {
    reconnectSettingsTransaction(transaction, live);
    return;
  }
  if (transaction.inFlight != SettingsOperation::none ||
      configMatches(transaction.confirmedLive, live)) {
    return;
  }
  transaction.confirmedLive = live;
  transaction.conflict = true;
  transaction.liveChangeFailed = false;
  transaction.diagnostic =
      "Live configuration changed outside this dialog; reopen it "
      "before applying settings";
}

void requestSettingsCancel(SettingsTransaction &transaction) {
  transaction.desiredLive = transaction.baselineLive;
  transaction.cancelRequested = true;
  transaction.conflict = false;
  transaction.liveChangeFailed = false;
  transaction.diagnostic.clear();
}

bool settingsTransactionCanApply(
    const SettingsTransaction &transaction) {
  return transaction.connected && !transaction.cancelRequested &&
         !transaction.conflict && !transaction.liveChangeFailed &&
         transaction.inFlight == SettingsOperation::none &&
         configMatches(transaction.desiredLive,
                       transaction.confirmedLive) &&
         !configMatches(transaction.desiredLive, transaction.saved);
}

bool settingsTransactionIsDirty(
    const SettingsTransaction &transaction) {
  return !configMatches(transaction.desiredLive, transaction.saved);
}

void completeSettingsPersistence(
    SettingsTransaction &transaction, bool success,
    std::string_view diagnostic) {
  if (!success) {
    transaction.diagnostic =
        diagnostic.empty() ? "Cannot persist startup configuration"
                           : std::string(diagnostic);
    return;
  }
  transaction.saved = transaction.desiredLive;
  transaction.baselineLive = transaction.confirmedLive;
  transaction.conflict = false;
  transaction.diagnostic.clear();
}

bool settingsTransactionShouldClose(
    const SettingsTransaction &transaction) {
  return transaction.cancelRequested && transaction.connected &&
         !transaction.conflict && !transaction.liveChangeFailed &&
         transaction.inFlight == SettingsOperation::none &&
         configMatches(transaction.confirmedLive,
                       transaction.baselineLive);
}

pipetune::StartupConfig startupConfigFromRuntime(
    const pipetune::ControlRuntimeStatus &status) {
  const auto presetFound =
      status.processingMode == pipetune::ProcessingMode::preset &&
      !status.activePreset.empty();
  return {
      .presetFound = presetFound,
      .presetPath =
          presetFound ? std::filesystem::path(status.activePreset)
                      : std::filesystem::path{},
      .preferredOutputFound = !status.preferredTarget.empty(),
      .preferredOutput = status.preferredTarget,
      .ratePolicy = status.configuredRatePolicy,
      .dspBackend = status.configuredDspBackend,
      .dspSimdVariant = status.configuredDspSimdVariant,
  };
}

} // namespace pipetune_gtk
