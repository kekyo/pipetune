#ifndef PIPETUNE_GTK_OUTPUT_OPERATION_H
#define PIPETUNE_GTK_OUTPUT_OPERATION_H

#include "application-state.h"
#include "control-client.h"

#include <cstdint>
#include <filesystem>
#include <string>

namespace pipetune_gtk {

/**
 * Identifies the user request awaiting a daemon output reply.
 */
struct OutputOperationRequest {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** True when the explicit preference should be removed. */
  bool clearPreference;
  /** Requested PipeWire node.name, or empty when clearing. */
  std::string target;
};

/**
 * Reports the effects of completing a GTK output operation.
 */
struct OutputOperationCompletion {
  /** True when the control subscription should reconnect. */
  bool reconnectRequired;
  /** True when the daemon confirmed the requested preference. */
  bool liveApplied;
  /** True when the confirmed preference was persisted. */
  bool persistenceApplied;
};

/**
 * Applies a daemon output reply and persists only a confirmed live change.
 *
 * A rejected or disconnected request leaves both the retained runtime output
 * state and startup preference unchanged. Persistence failure keeps the live
 * engine state and records an explicit partial-success diagnostic.
 *
 * @param state Application state to update and take out of pending mode.
 * @param reply Completed asynchronous daemon request.
 * @param request Preference and configuration path awaiting confirmation.
 * @param receivedAtMonotonicMilliseconds Reply receipt time.
 * @return Reconnect and two-phase completion state.
 */
OutputOperationCompletion completeOutputOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const OutputOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds);

} // namespace pipetune_gtk

#endif
