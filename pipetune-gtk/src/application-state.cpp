#include "application-state.h"

namespace pipetune_gtk {

ApplicationState initialApplicationState() {
  return {
      .connection = ControlConnectionState::disconnected,
      .hasRuntimeStatus = false,
      .runtime =
          {
              .activePreset = {},
              .activePluginCount = 0,
              .selectedTarget = {},
              .defaultSinkActive = false,
              .overrunFrames = 0,
              .underrunFrames = 0,
              .processingErrors = 0,
          },
      .warnings = {},
      .diagnostic = {},
      .operationPending = false,
  };
}

void markControlConnecting(ApplicationState &state) {
  state.connection = ControlConnectionState::connecting;
  state.diagnostic.clear();
}

void applyControlResponse(
    ApplicationState &state,
    const pipetune::ControlResponseParseResult &response) {
  if (!response.valid) {
    state.diagnostic = response.error;
    return;
  }
  state.connection = ControlConnectionState::connected;
  if (!response.success) {
    state.diagnostic = response.error;
    return;
  }

  state.hasRuntimeStatus = true;
  state.runtime = response.status;
  if (response.kind == pipetune::ControlResponseKind::response) {
    state.warnings = response.warnings;
    state.diagnostic.clear();
  }
}

void markControlDisconnected(ApplicationState &state,
                             std::string_view diagnostic) {
  state.connection = ControlConnectionState::disconnected;
  state.operationPending = false;
  state.diagnostic = diagnostic;
}

void setControlOperationPending(ApplicationState &state, bool pending) {
  state.operationPending = pending;
}

void setControlDiagnostic(ApplicationState &state,
                          std::string_view diagnostic) {
  state.diagnostic = diagnostic;
}

void clearControlNotice(ApplicationState &state) {
  state.warnings.clear();
  state.diagnostic.clear();
}

TrayVisualState trayVisualState(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected) {
    return TrayVisualState::disconnected;
  }
  if (!state.hasRuntimeStatus || !state.diagnostic.empty() ||
      !state.warnings.empty() || !state.runtime.defaultSinkActive ||
      state.runtime.selectedTarget.empty() ||
      state.runtime.overrunFrames != 0 ||
      state.runtime.underrunFrames != 0 ||
      state.runtime.processingErrors != 0) {
    return TrayVisualState::attention;
  }
  return TrayVisualState::active;
}

} // namespace pipetune_gtk
