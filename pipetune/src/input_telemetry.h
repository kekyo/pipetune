#ifndef PIPETUNE_INPUT_TELEMETRY_H
#define PIPETUNE_INPUT_TELEMETRY_H

#include <atomic>
#include <cstdint>

namespace pipetune {

/**
 * Stores counters written from the PipeWire capture callback.
 */
struct InputTelemetry {
  /** Total number of valid PCM frames received from PipeWire. */
  std::atomic<std::uint64_t> framesReceived;
  /** Monotonic timestamp of the most recent non-empty capture buffer. */
  std::atomic<std::int64_t> lastReceivedMonotonicNanoseconds;

  /** Creates empty input telemetry. */
  InputTelemetry() noexcept;
};

/**
 * Describes a thread-safe input telemetry snapshot.
 */
struct InputTelemetrySnapshot {
  /** Total number of valid PCM frames received from PipeWire. */
  std::uint64_t framesReceived;
  /** Estimated Unix time of the latest received frame, or zero before input. */
  std::uint64_t lastReceivedUnixMilliseconds;
};

/**
 * Records one valid PipeWire capture buffer without allocating or locking.
 *
 * @param telemetry Telemetry counters to update.
 * @param frameCount Number of received PCM frames; zero is ignored.
 * @param monotonicNanoseconds Current monotonic time in nanoseconds.
 */
void recordInputFrames(InputTelemetry &telemetry, std::uint32_t frameCount,
                       std::int64_t monotonicNanoseconds) noexcept;

/**
 * Takes a telemetry snapshot and maps its monotonic timestamp to wall time.
 *
 * @param telemetry Telemetry counters to inspect.
 * @param currentMonotonicNanoseconds Current monotonic time in nanoseconds.
 * @param currentUnixMilliseconds Current Unix wall time in milliseconds.
 * @return Current frame counter and estimated latest receive time.
 */
InputTelemetrySnapshot snapshotInputTelemetry(
    const InputTelemetry &telemetry, std::int64_t currentMonotonicNanoseconds,
    std::uint64_t currentUnixMilliseconds) noexcept;

} // namespace pipetune

#endif
