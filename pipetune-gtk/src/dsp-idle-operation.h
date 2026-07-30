#ifndef PIPETUNE_GTK_DSP_IDLE_OPERATION_H
#define PIPETUNE_GTK_DSP_IDLE_OPERATION_H

#include "application-state.h"
#include "control-client.h"

#include "pipetune/dsp_idle.h"

#include <cstdint>
#include <filesystem>

namespace pipetune_gtk {

/**
 * Identifies the user DSP idle policy awaiting completion.
 */
struct DspIdleOperationRequest {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Requested conservative tail threshold or exact-zero policy. */
  pipetune::DspIdlePolicy policy;
};

/**
 * Reports the effects of completing a GTK DSP idle operation.
 */
struct DspIdleOperationCompletion {
  /** True when the control subscription should reconnect. */
  bool reconnectRequired;
  /** True when the daemon confirmed the requested live policy. */
  bool liveApplied;
  /** True when the requested policy was persisted. */
  bool persistenceApplied;
};

/**
 * Applies a daemon idle-policy reply and persists a confirmed live change.
 *
 * A transport failure persists the user's explicit selection for the next
 * daemon start. A rejected or unconfirmed daemon reply does not overwrite the
 * startup policy. Persistence failure retains a confirmed live change and
 * records an explicit partial-success diagnostic.
 *
 * @param state Application state to update and take out of pending mode.
 * @param reply Completed asynchronous daemon request.
 * @param request Policy and configuration path awaiting completion.
 * @param receivedAtMonotonicMilliseconds Reply receipt time.
 * @return Reconnect and two-phase completion state.
 */
DspIdleOperationCompletion completeDspIdleOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const DspIdleOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds);

/**
 * Persists a DSP idle policy while no daemon connection is available.
 *
 * @param state Application state that receives the persistence diagnostic.
 * @param request Policy and configuration path to store.
 * @return Offline persistence completion state.
 */
DspIdleOperationCompletion persistDspIdleOperationForNextStart(
    ApplicationState &state, const DspIdleOperationRequest &request);

} // namespace pipetune_gtk

#endif
