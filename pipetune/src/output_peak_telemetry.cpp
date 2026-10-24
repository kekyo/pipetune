/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output_peak_telemetry.h"

#include <algorithm>
#include <cmath>

namespace pipetune {

static_assert(std::atomic<float>::is_always_lock_free);

OutputPeakTelemetry::OutputPeakTelemetry() noexcept {
  for (auto &peak : pending) peak.store(0.0F, std::memory_order_relaxed);
  for (auto &peak : published) peak.store(0.0F, std::memory_order_relaxed);
}

void recordOutputPeak(OutputPeakTelemetry &telemetry, std::size_t channel,
                      std::span<const float> samples) noexcept {
  if (channel >= kMaximumOutputPeakChannels) return;
  auto peak = 0.0F;
  for (const auto sample : samples) {
    if (std::isfinite(sample)) peak = std::max(peak, std::abs(sample));
  }
  auto current = telemetry.pending[channel].load(std::memory_order_relaxed);
  while (current < peak && !telemetry.pending[channel].compare_exchange_weak(
                               current, peak, std::memory_order_relaxed)) {}
}

bool publishOutputPeaks(OutputPeakTelemetry &telemetry) noexcept {
  auto changed = false;
  for (auto channel = std::size_t{0}; channel < kMaximumOutputPeakChannels;
       ++channel) {
    const auto peak = telemetry.pending[channel].exchange(
        0.0F, std::memory_order_relaxed);
    changed |= peak != telemetry.published[channel].load(std::memory_order_relaxed);
    telemetry.published[channel].store(peak, std::memory_order_release);
  }
  return changed;
}

void clearOutputPeaks(OutputPeakTelemetry &telemetry) noexcept {
  for (auto &peak : telemetry.pending)
    peak.store(0.0F, std::memory_order_relaxed);
  for (auto &peak : telemetry.published)
    peak.store(0.0F, std::memory_order_release);
}

std::vector<float> snapshotOutputPeaks(const OutputPeakTelemetry &telemetry,
                                       std::size_t channelCount) {
  if (channelCount == 0 || channelCount > kMaximumOutputPeakChannels) return {};
  auto peaks = std::vector<float>(channelCount);
  for (auto channel = std::size_t{0}; channel < channelCount; ++channel)
    peaks[channel] = telemetry.published[channel].load(std::memory_order_acquire);
  return peaks;
}

} // namespace pipetune
