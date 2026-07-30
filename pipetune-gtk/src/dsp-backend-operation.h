#ifndef PIPETUNE_GTK_DSP_BACKEND_OPERATION_H
#define PIPETUNE_GTK_DSP_BACKEND_OPERATION_H

#include "application-state.h"
#include "control-client.h"

#include "pipetune/dsp_backend.h"

#include <cstdint>
#include <filesystem>

namespace pipetune_gtk {

/**
 * Identifies the user DSP backend request awaiting completion.
 */
struct DspBackendOperationRequest {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Requested scalar compatibility or SIMD acceleration backend. */
  pipetune::DspBackendKind kind;
  /** Requested automatic or pinned SIMD dispatch preference. */
  pipetune::DspSimdVariant simdVariant =
      pipetune::DspSimdVariant::automatic;
};

/**
 * Reports the effects of completing a GTK DSP backend operation.
 */
struct DspBackendOperationCompletion {
  /** True when the control subscription should reconnect. */
  bool reconnectRequired;
  /** True when the daemon confirmed the requested live backend. */
  bool liveApplied;
  /** True when the requested backend was persisted. */
  bool persistenceApplied;
};

/**
 * Applies a daemon backend reply and persists only a confirmed live change.
 *
 * A transport failure validates the packaged SO and CPU support before
 * persisting the user's selection for the next daemon start. Automatic
 * dispatch may confirm a lower SIMD tier with a diagnostic. A rejection,
 * scalar fallback, or pinned-tier mismatch does not overwrite startup
 * configuration.
 *
 * @param state Application state to update and take out of pending mode.
 * @param reply Completed asynchronous daemon request.
 * @param request Backend and configuration path awaiting completion.
 * @param receivedAtMonotonicMilliseconds Reply receipt time.
 * @return Reconnect and two-phase completion state.
 */
DspBackendOperationCompletion completeDspBackendOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const DspBackendOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds);

/**
 * Validates and persists a backend while no daemon connection is available.
 *
 * @param state Application state that receives the result diagnostic.
 * @param request Backend and configuration path to validate and store.
 * @return Offline persistence completion state.
 */
DspBackendOperationCompletion
persistDspBackendOperationForNextStart(
    ApplicationState &state,
    const DspBackendOperationRequest &request);

} // namespace pipetune_gtk

#endif
