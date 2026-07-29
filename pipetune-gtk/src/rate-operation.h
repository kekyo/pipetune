#ifndef PIPETUNE_GTK_RATE_OPERATION_H
#define PIPETUNE_GTK_RATE_OPERATION_H

#include "application-state.h"
#include "control-client.h"

#include "pipetune/sample_rate.h"

#include <cstdint>
#include <filesystem>

namespace pipetune_gtk {

/**
 * Identifies the user PCM rate request awaiting completion.
 */
struct RateOperationRequest {
  /** Canonical startup configuration path. */
  std::filesystem::path configPath;
  /** Requested Max/fixed and suggest/force policy. */
  pipetune::SampleRatePolicy policy;
};

/**
 * Reports the effects of completing a GTK PCM rate operation.
 */
struct RateOperationCompletion {
  /** True when the control subscription should reconnect. */
  bool reconnectRequired;
  /** True when the daemon confirmed the requested live policy. */
  bool liveApplied;
  /** True when the requested policy was persisted. */
  bool persistenceApplied;
};

/**
 * Applies a daemon rate reply and persists only a confirmed live change.
 *
 * A transport failure persists the user's explicit selection for the next
 * daemon start. A rejected or incomplete daemon reply does not overwrite the
 * startup policy. Persistence failure retains a confirmed live change and
 * records an explicit partial-success diagnostic.
 *
 * @param state Application state to update and take out of pending mode.
 * @param reply Completed asynchronous daemon request.
 * @param request Policy and configuration path awaiting completion.
 * @param receivedAtMonotonicMilliseconds Reply receipt time.
 * @return Reconnect and two-phase completion state.
 */
RateOperationCompletion completeRateOperation(
    ApplicationState &state, const ControlClientReply &reply,
    const RateOperationRequest &request,
    std::int64_t receivedAtMonotonicMilliseconds);

/**
 * Persists a policy while no daemon connection is available.
 *
 * @param state Application state that receives the persistence diagnostic.
 * @param request Policy and configuration path to store.
 * @return Offline persistence completion state.
 */
RateOperationCompletion persistRateOperationForNextStart(
    ApplicationState &state, const RateOperationRequest &request);

} // namespace pipetune_gtk

#endif
