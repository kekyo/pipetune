#ifndef PIPETUNE_GTK_APPLICATION_STATE_H
#define PIPETUNE_GTK_APPLICATION_STATE_H

#include "pipetune/control_protocol.h"

#include <cstdint>
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
 * Stores the GUI's input frame-rate measurement state.
 */
struct InputRateState {
  /** True after a compatible cumulative-frame baseline was received. */
  bool hasBaseline;
  /** Cumulative frame count at the current measurement baseline. */
  std::uint64_t baselineFrames;
  /** Monotonic time of the current measurement baseline in milliseconds. */
  std::int64_t baselineMonotonicMilliseconds;
  /** Sample format associated with the current measurement baseline. */
  std::string baselineSampleFormat;
  /** Sample rate associated with the current measurement baseline. */
  std::uint32_t baselineSampleRate;
  /** Channel count associated with the current measurement baseline. */
  std::uint32_t baselineChannelCount;
  /** True after at least one sufficiently long interval was measured. */
  bool hasRate;
  /** Most recently measured input frames per second. */
  double framesPerSecond;
};

/**
 * Stores the latest interval average for native EffeTune processing.
 */
struct DspTimingState {
  /** True after cumulative DSP counters established a baseline. */
  bool hasBaseline;
  /** Cumulative processed frames at the current baseline. */
  std::uint64_t baselineFrames;
  /** Cumulative processing nanoseconds at the current baseline. */
  std::uint64_t baselineNanoseconds;
  /** True when the latest active interval contained DSP frames. */
  bool hasAverage;
  /** Latest active-interval DSP processing nanoseconds per frame. */
  double nanosecondsPerFrame;
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
  /** True while an explicit preset, output, rate, or backend operation runs. */
  bool operationPending;
  /** Input frame-rate baseline and most recent derived value. */
  InputRateState inputRate;
  /** Native DSP timing baseline and most recent interval average. */
  DspTimingState dspTiming;
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
 * Any runtime status from a previous connection is no longer considered
 * current.
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
 * @param receivedAtMonotonicMilliseconds Monotonic receipt time in milliseconds.
 */
void applyControlResponse(
    ApplicationState &state,
    const pipetune::ControlResponseParseResult &response,
    std::int64_t receivedAtMonotonicMilliseconds);

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
 * Returns whether a confirmed preset pipeline is currently applied.
 *
 * @param state State to inspect.
 * @return True only while a connected daemon confirms preset processing.
 */
bool isPresetApplied(const ApplicationState &state);

/**
 * Selects the tray icon semantics for the current state.
 *
 * @param state State to inspect.
 * @return Active, attention, or disconnected.
 */
TrayVisualState trayVisualState(const ApplicationState &state);

} // namespace pipetune_gtk

#endif
