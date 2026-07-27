#ifndef PIPETUNE_GTK_APPLICATION_STATE_H
#define PIPETUNE_GTK_APPLICATION_STATE_H

#include "pipetune/control_protocol.h"

#include <string>
#include <string_view>
#include <vector>

namespace pipetune_gtk {

/**
 * Identifies the GUI's control-socket lifecycle.
 */
enum class ControlConnectionState {
  /** No usable daemon connection exists. */
  disconnected,
  /** An asynchronous subscription is being established. */
  connecting,
  /** A valid daemon status has been received. */
  connected
};

/**
 * Identifies the semantic tray icon to display.
 */
enum class TrayVisualState {
  /** PipeTune is connected and healthy. */
  active,
  /** PipeTune is connected but needs user attention. */
  attention,
  /** The PipeTune daemon is unavailable. */
  disconnected
};

/**
 * Stores display-independent GUI state.
 */
struct ApplicationState {
  /** Current control connection lifecycle. */
  ControlConnectionState connection;
  /** True after at least one complete runtime status was received. */
  bool hasRuntimeStatus;
  /** Most recently received runtime status. */
  pipetune::ControlRuntimeStatus runtime;
  /** Warnings from the most recent explicit control request. */
  std::vector<pipetune::ControlWarning> warnings;
  /** Transport, protocol, remote, or persistence diagnostic. */
  std::string diagnostic;
  /** True while an explicit preset operation is running. */
  bool operationPending;
};

/**
 * Creates the disconnected initial GUI state.
 *
 * @return Initial state with no runtime status.
 */
ApplicationState initialApplicationState();

/**
 * Marks an in-progress subscription connection.
 *
 * @param state State to update.
 */
void markControlConnecting(ApplicationState &state);

/**
 * Applies a parsed reply or subscribed status event.
 *
 * Status events update runtime fields but intentionally preserve warnings from
 * an explicit load request until the user dismisses them.
 *
 * @param state State to update.
 * @param response Parsed control message.
 */
void applyControlResponse(
    ApplicationState &state,
    const pipetune::ControlResponseParseResult &response);

/**
 * Marks the daemon connection unavailable.
 *
 * @param state State to update.
 * @param diagnostic Human-readable transport diagnostic.
 */
void markControlDisconnected(ApplicationState &state,
                             std::string_view diagnostic);

/**
 * Sets whether an explicit operation is in flight.
 *
 * @param state State to update.
 * @param pending True while the operation is pending.
 */
void setControlOperationPending(ApplicationState &state, bool pending);

/**
 * Records a local persistence or validation diagnostic.
 *
 * @param state State to update.
 * @param diagnostic Human-readable diagnostic.
 */
void setControlDiagnostic(ApplicationState &state,
                          std::string_view diagnostic);

/**
 * Clears request warnings and the current diagnostic.
 *
 * @param state State to update.
 */
void clearControlNotice(ApplicationState &state);

/**
 * Selects the tray icon semantics for the current state.
 *
 * @param state State to inspect.
 * @return Active, attention, or disconnected.
 */
TrayVisualState trayVisualState(const ApplicationState &state);

} // namespace pipetune_gtk

#endif
