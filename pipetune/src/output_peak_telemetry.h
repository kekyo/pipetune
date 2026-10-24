/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_PEAK_TELEMETRY_H
#define PIPETUNE_OUTPUT_PEAK_TELEMETRY_H

#include <array>
#include <atomic>
#include <cstddef>
#include <span>
#include <vector>

namespace pipetune {

/** Maximum number of logical PCM output channels. */
constexpr auto kMaximumOutputPeakChannels = std::size_t{16};

/** Lock-free peak storage shared by the audio, main-loop, and control threads. */
struct OutputPeakTelemetry {
  /** Peak amplitudes accumulated since the last publication. */
  std::array<std::atomic<float>, kMaximumOutputPeakChannels> pending;
  /** Peak amplitudes in the last complete publication interval. */
  std::array<std::atomic<float>, kMaximumOutputPeakChannels> published;

  /** Initializes every channel to silence. */
  OutputPeakTelemetry() noexcept;
};

/**
 * Records the largest absolute finite PCM sample in one output channel.
 *
 * @param telemetry Shared meter state.
 * @param channel Zero-based logical output channel.
 * @param samples Final samples delivered to the PipeWire output stream.
 */
void recordOutputPeak(OutputPeakTelemetry &telemetry, std::size_t channel,
                      std::span<const float> samples) noexcept;

/**
 * Publishes interval maxima, clearing channels with no new samples.
 *
 * @param telemetry Shared meter state.
 * @return True if at least one published peak changed.
 */
bool publishOutputPeaks(OutputPeakTelemetry &telemetry) noexcept;

/**
 * Copies the most recently published peaks in logical channel order.
 *
 * @param telemetry Shared meter state.
 * @param channelCount Number of output channels, at most sixteen.
 * @return Finite nonnegative linear amplitudes; empty for an invalid count.
 */
std::vector<float> snapshotOutputPeaks(const OutputPeakTelemetry &telemetry,
                                       std::size_t channelCount);

} // namespace pipetune

#endif
