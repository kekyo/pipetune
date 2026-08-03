#ifndef PIPETUNE_GTK_SETTINGS_TRANSACTION_H
#define PIPETUNE_GTK_SETTINGS_TRANSACTION_H

#include "pipetune/control_protocol.h"
#include "pipetune/startup_config.h"

#include <string>
#include <string_view>

namespace pipetune_gtk {

/**
 * Identifies one serialized live setting operation.
 */
enum class SettingsOperation {
  /** No live operation is currently required. */
  none,
  /** Replace or clear the preferred output. */
  output,
  /** Replace the sample-rate policy. */
  rate,
  /** Replace the DSP backend and dispatch variant. */
  dspBackend,
  /** Load a preset or enter bypass. */
  processing
};

/**
 * Stores one dialog-wide settings transaction.
 */
struct SettingsTransaction {
  /** Configuration loaded from persistent storage. */
  pipetune::StartupConfig saved;
  /** Live state captured when the dialog opened or Apply succeeded. */
  pipetune::StartupConfig baselineLive;
  /** Latest state selected by the user. */
  pipetune::StartupConfig desiredLive;
  /** Latest state confirmed by the daemon. */
  pipetune::StartupConfig confirmedLive;
  /** Operation currently awaiting a daemon reply. */
  SettingsOperation inFlight;
  /** Desired snapshot captured when the current operation started. */
  pipetune::StartupConfig inFlightTarget;
  /** True while a usable daemon connection exists. */
  bool connected;
  /** True after Cancel requested restoration of baselineLive. */
  bool cancelRequested;
  /** True after an unexpected external live configuration change. */
  bool conflict;
  /** True after a live operation failed and awaits another user action. */
  bool liveChangeFailed;
  /** Most recent transaction diagnostic. */
  std::string diagnostic;
};

/**
 * Creates a settings transaction after startup and live state are available.
 *
 * @param saved Configuration loaded from persistent storage.
 * @param live Complete live state reported by the daemon.
 * @param connected True when live requests may be issued immediately.
 * @return Initialized transaction with no operation in flight.
 */
SettingsTransaction beginSettingsTransaction(
    const pipetune::StartupConfig &saved,
    const pipetune::StartupConfig &live, bool connected);

/**
 * Replaces the desired settings and permits a failed operation to retry.
 *
 * @param transaction Transaction to update.
 * @param desired Complete user-selected live state.
 */
void editSettingsTransaction(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &desired);

/**
 * Selects the next required live operation in dependency order.
 *
 * @param transaction Transaction to inspect.
 * @return Output, rate, backend, processing, or none.
 */
SettingsOperation
nextSettingsOperation(const SettingsTransaction &transaction);

/**
 * Marks the selected next operation as in flight.
 *
 * @param transaction Transaction to update.
 * @param operation Operation returned by nextSettingsOperation().
 * @return True when the operation was accepted.
 */
bool beginSettingsOperation(SettingsTransaction &transaction,
                            SettingsOperation operation);

/**
 * Completes the current operation from one verified daemon reply.
 *
 * @param transaction Transaction to update.
 * @param success True when the daemon accepted the request.
 * @param confirmed Complete live state returned by the daemon.
 * @param diagnostic Failure diagnostic, or empty after success.
 */
void completeSettingsOperation(
    SettingsTransaction &transaction, bool success,
    const pipetune::StartupConfig &confirmed,
    std::string_view diagnostic);

/**
 * Marks the transaction read-only after losing the daemon connection.
 *
 * @param transaction Transaction to update.
 * @param diagnostic Connection diagnostic displayed by the dialog.
 */
void markSettingsDisconnected(SettingsTransaction &transaction,
                              std::string_view diagnostic);

/**
 * Re-establishes confirmed live state and resumes pending edits.
 *
 * @param transaction Transaction to update.
 * @param live Complete state received after reconnection.
 */
void reconnectSettingsTransaction(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &live);

/**
 * Observes a subscribed live state change outside an explicit operation.
 *
 * An unexpected difference is treated as an external configuration conflict.
 *
 * @param transaction Transaction to update.
 * @param live Complete subscribed live state.
 */
void observeSettingsRuntime(
    SettingsTransaction &transaction,
    const pipetune::StartupConfig &live);

/**
 * Requests restoration of the baseline captured when the dialog opened.
 *
 * @param transaction Transaction to update.
 */
void requestSettingsCancel(SettingsTransaction &transaction);

/**
 * Reports whether Apply may atomically persist the desired snapshot.
 *
 * @param transaction Transaction to inspect.
 * @return True only after all live changes are confirmed and remain dirty.
 */
bool settingsTransactionCanApply(
    const SettingsTransaction &transaction);

/**
 * Reports whether desired settings differ from persistent storage.
 *
 * @param transaction Transaction to inspect.
 * @return True while Apply has meaningful work.
 */
bool settingsTransactionIsDirty(
    const SettingsTransaction &transaction);

/**
 * Records completion of the dialog-wide persistence step.
 *
 * Successful persistence establishes new saved and live baselines without
 * closing the dialog.
 *
 * @param transaction Transaction to update.
 * @param success True when the complete snapshot was stored atomically.
 * @param diagnostic Persistence diagnostic, or empty after success.
 */
void completeSettingsPersistence(
    SettingsTransaction &transaction, bool success,
    std::string_view diagnostic);

/**
 * Reports whether a requested Cancel rollback is fully confirmed.
 *
 * @param transaction Transaction to inspect.
 * @return True when the window may safely hide.
 */
bool settingsTransactionShouldClose(
    const SettingsTransaction &transaction);

/**
 * Converts complete daemon status into the settings transaction value type.
 *
 * @param status Runtime status received from the control socket.
 * @return Live preset, output, rate, and backend choices.
 */
pipetune::StartupConfig startupConfigFromRuntime(
    const pipetune::ControlRuntimeStatus &status);

} // namespace pipetune_gtk

#endif
