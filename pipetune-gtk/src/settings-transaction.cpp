/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "settings-transaction.h"

#include <algorithm>
#include <string>

namespace pipetune_gtk {

static bool outputProfilesMatch(
    const std::vector<pipetune::OutputProfileSelection> &left,
    const std::vector<pipetune::OutputProfileSelection> &right) {
  return std::is_permutation(left.begin(), left.end(), right.begin(), right.end());
}

static bool configMatches(const pipetune::StartupConfig &left,
                          const pipetune::StartupConfig &right) {
  // The SFZ budget is a startup-only choice, independent of live confirmation.
  return left.presetFound == right.presetFound &&
         left.presetPath == right.presetPath &&
         left.ratePolicy == right.ratePolicy &&
         left.dspBackend == right.dspBackend &&
         left.dspSimdVariant == right.dspSimdVariant &&
         left.dspIdlePolicy == right.dspIdlePolicy &&
         left.outputConfiguration == right.outputConfiguration;
}

static bool operationMatches(
    SettingsOperation operation,
    const pipetune::StartupConfig &left,
    const pipetune::StartupConfig &right) {
  switch (operation) {
  case SettingsOperation::none:
    return true;
  case SettingsOperation::rate:
    return left.ratePolicy == right.ratePolicy;
  case SettingsOperation::dspBackend:
    return left.dspBackend == right.dspBackend &&
           left.dspSimdVariant == right.dspSimdVariant;
  case SettingsOperation::dspIdle:
    return left.dspIdlePolicy == right.dspIdlePolicy;
  case SettingsOperation::output:
    return left.outputConfiguration == right.outputConfiguration &&
           left.presetFound == right.presetFound && left.presetPath == right.presetPath;
  case SettingsOperation::processing:
    return left.presetFound == right.presetFound &&
           left.presetPath == right.presetPath;
  }
  return false;
}

static std::string confirmationDiagnostic(SettingsOperation operation) {
  switch (operation) {
  case SettingsOperation::rate:
    return "Daemon did not confirm the requested sample-rate policy";
  case SettingsOperation::dspBackend:
    return "Daemon did not confirm the requested DSP backend";
  case SettingsOperation::dspIdle:
    return "Daemon did not confirm the requested DSP suspension policy";
  case SettingsOperation::output:
    return "Daemon did not confirm the requested outputs and processing mode";
  case SettingsOperation::processing:
    return "Daemon did not confirm the requested processing mode";
  case SettingsOperation::none:
    return "No settings operation was in flight";
  }
  return "Daemon did not confirm the requested setting";
}

SettingsTransaction beginSettingsTransaction(
    const pipetune::StartupConfig &saved,
    const pipetune::StartupConfig &live, std::uint64_t liveRevision,
    bool connected, std::span<const pipetune::OutputProfileSelection> outputProfiles) {
  auto desired = live;
  desired.sfzMaxSizeMiB = saved.sfzMaxSizeMiB;
  return {
      .saved = saved,
      .baselineLive = live,
      .desiredLive = std::move(desired),
      .confirmedLive = live,
      .confirmedRevision = liveRevision,
      .inFlight = SettingsOperation::none,
      .inFlightTarget = live,
      .connected = connected,
      .cancelRequested = false,
      .conflict = false,
      .liveChangeFailed = false,
      .diagnostic = {},
      .baselineOutputProfiles = {outputProfiles.begin(), outputProfiles.end()},
      .desiredOutputProfiles = {outputProfiles.begin(), outputProfiles.end()},
      .confirmedOutputProfiles = {outputProfiles.begin(), outputProfiles.end()},
  };
}

void restoreSettingsDefaults(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &defaults) {
  transaction.desiredLive = defaults;
  transaction.cancelRequested = false;
  transaction.conflict = false;
  transaction.liveChangeFailed = false;
  transaction.diagnostic.clear();
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
  // Mode selection precedes the first device check, so an incomplete draft
  // must remain editable without sending a request the daemon will reject.
  if (!pipetune::validateOutputConfiguration(
           transaction.desiredLive.outputConfiguration).empty()) {
    return SettingsOperation::none;
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
  if (!operationMatches(SettingsOperation::dspIdle,
                        transaction.confirmedLive,
                        transaction.desiredLive)) {
    return SettingsOperation::dspIdle;
  }
  if (transaction.confirmedLive.outputConfiguration != transaction.desiredLive.outputConfiguration ||
      !outputProfilesMatch(transaction.confirmedOutputProfiles, transaction.desiredOutputProfiles)) {
    // Output width and preset may be mutually incompatible with the current
    // configuration. Restore them together instead of ordering partial edits.
    return SettingsOperation::output;
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
  transaction.inFlightOutputProfiles = transaction.desiredOutputProfiles;
  return true;
}

void completeSettingsOperation(
    SettingsTransaction &transaction, bool success,
    const pipetune::StartupConfig &confirmed,
    std::uint64_t confirmedRevision,
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
  transaction.confirmedRevision = confirmedRevision;
  if (operation == SettingsOperation::output)
    transaction.confirmedOutputProfiles = transaction.inFlightOutputProfiles;
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

static bool observeOutputProfiles(SettingsTransaction &transaction,
    std::span<const pipetune::OutputProfileSelection> profiles) {
  auto changed = false;
  for (const auto &profile : profiles) {
    const auto known = std::find_if(transaction.confirmedOutputProfiles.begin(),
        transaction.confirmedOutputProfiles.end(),
        [&profile](const auto &entry) { return entry.card == profile.card; });
    if (known == transaction.confirmedOutputProfiles.end()) {
      // A card first seen after opening the dialog still needs a checkpoint.
      transaction.baselineOutputProfiles.push_back(profile);
      transaction.desiredOutputProfiles.push_back(profile);
      transaction.confirmedOutputProfiles.push_back(profile);
    } else if (*known != profile) {
      *known = profile;
      changed = true;
    }
  }
  return changed;
}

void reconnectSettingsTransaction(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &live,
    std::uint64_t liveRevision,
    std::span<const pipetune::OutputProfileSelection> outputProfiles) {
  observeOutputProfiles(transaction, outputProfiles);
  transaction.connected = true;
  transaction.confirmedLive = live;
  transaction.confirmedRevision = liveRevision;
  transaction.inFlight = SettingsOperation::none;
  transaction.inFlightTarget = live;
  transaction.conflict = false;
  transaction.liveChangeFailed = false;
  transaction.diagnostic.clear();
}

void observeSettingsRuntime(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &live,
    std::uint64_t liveRevision,
    std::span<const pipetune::OutputProfileSelection> outputProfiles) {
  if (!transaction.connected) {
    reconnectSettingsTransaction(transaction, live, liveRevision, outputProfiles);
    return;
  }
  if (transaction.inFlight != SettingsOperation::none ||
      liveRevision < transaction.confirmedRevision) {
    return;
  }
  const auto profilesChanged = observeOutputProfiles(transaction, outputProfiles);
  if (!profilesChanged && configMatches(transaction.confirmedLive, live)) {
    transaction.confirmedRevision = liveRevision;
    return;
  }
  transaction.confirmedLive = live;
  transaction.confirmedRevision = liveRevision;
  transaction.conflict = true;
  transaction.liveChangeFailed = false;
  transaction.diagnostic =
      "Live configuration changed outside this dialog; reopen it "
      "before applying settings";
}

void requestSettingsCancel(SettingsTransaction &transaction) {
  transaction.desiredLive = transaction.baselineLive;
  transaction.desiredOutputProfiles = transaction.baselineOutputProfiles;
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
         outputProfilesMatch(transaction.desiredOutputProfiles,
                             transaction.confirmedOutputProfiles) &&
         settingsTransactionIsDirty(transaction);
}

bool settingsTransactionIsDirty(
    const SettingsTransaction &transaction) {
  return !configMatches(transaction.desiredLive, transaction.saved) ||
         !outputProfilesMatch(transaction.desiredOutputProfiles, transaction.baselineOutputProfiles) ||
         transaction.desiredLive.sfzMaxSizeMiB != transaction.saved.sfzMaxSizeMiB;
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
  transaction.baselineOutputProfiles = transaction.confirmedOutputProfiles;
  transaction.conflict = false;
  transaction.diagnostic.clear();
}

bool settingsTransactionShouldClose(
    const SettingsTransaction &transaction) {
  return transaction.cancelRequested && transaction.connected &&
         !transaction.conflict && !transaction.liveChangeFailed &&
         transaction.inFlight == SettingsOperation::none &&
         configMatches(transaction.confirmedLive,
                       transaction.baselineLive) &&
         outputProfilesMatch(transaction.confirmedOutputProfiles,
                             transaction.baselineOutputProfiles);
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
      .ratePolicy = status.configuredRatePolicy,
      .dspBackend = status.configuredDspBackend,
      .dspSimdVariant = status.configuredDspSimdVariant,
      .dspIdlePolicy = status.dspIdlePolicy,
      .outputConfiguration = status.outputConfiguration,
      .sfzMaxSizeMiB = status.sfzMaxSizeMiB,
  };
}

} // namespace pipetune_gtk
