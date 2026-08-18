/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_latency.h"

#include <spa/param/latency-utils.h>

#include <cstdint>

namespace pipetune {

PipeWireProcessLatencyCalculation calculatePipeWireProcessLatency(
    std::uint32_t dspLatencyFrames, std::uint32_t dspSampleRate,
    std::uint32_t bridgeLatencyFrames,
    std::uint32_t streamSampleRate) noexcept {
  if (dspSampleRate == 0 || streamSampleRate == 0) {
    return {.frames = 0, .valid = false};
  }
  const auto scaledDspFrames =
      static_cast<std::uint64_t>(dspLatencyFrames) * streamSampleRate;
  auto streamDspFrames = scaledDspFrames / dspSampleRate;
  if (scaledDspFrames % dspSampleRate != 0) {
    ++streamDspFrames;
  }
  const auto totalFrames =
      streamDspFrames + bridgeLatencyFrames;
  if (totalFrames > UINT32_MAX) {
    return {.frames = 0, .valid = false};
  }
  return {.frames = static_cast<std::uint32_t>(totalFrames),
          .valid = true};
}

PipeWireLatencyState::PipeWireLatencyState() noexcept
    : latencies_{}, processLatency_{} {
  latencies_[SPA_DIRECTION_INPUT].direction = SPA_DIRECTION_INPUT;
  latencies_[SPA_DIRECTION_OUTPUT].direction = SPA_DIRECTION_OUTPUT;
}

PipeWirePortLatencyUpdate PipeWireLatencyState::updatePortLatency(
    const spa_pod *parameter) noexcept {
  if (parameter == nullptr) {
    return {.valid = false, .direction = SPA_DIRECTION_INPUT};
  }
  auto latency = spa_latency_info{};
  if (spa_latency_parse(parameter, &latency) < 0 ||
      (latency.direction != SPA_DIRECTION_INPUT &&
       latency.direction != SPA_DIRECTION_OUTPUT)) {
    return {.valid = false, .direction = SPA_DIRECTION_INPUT};
  }
  latencies_[latency.direction] = latency;
  return {.valid = true, .direction = latency.direction};
}

void PipeWireLatencyState::setProcessLatencyFrames(
    std::uint32_t frames) noexcept {
  processLatency_ = {};
  processLatency_.rate = frames;
}

spa_process_latency_info
PipeWireLatencyState::processLatency() const noexcept {
  return processLatency_;
}

spa_latency_info PipeWireLatencyState::propagatedLatency(
    spa_direction direction) const noexcept {
  if (direction != SPA_DIRECTION_INPUT &&
      direction != SPA_DIRECTION_OUTPUT) {
    auto latency = spa_latency_info{};
    latency.direction = SPA_DIRECTION_OUTPUT;
    return latency;
  }
  auto latency = latencies_[direction];
  spa_process_latency_info_add(&processLatency_, &latency);
  return latency;
}

bool PipeWireLatencyState::processLatencyMatches(
    const spa_pod *parameter) const noexcept {
  if (parameter == nullptr) {
    return false;
  }
  auto parsed = spa_process_latency_info{};
  return spa_process_latency_parse(parameter, &parsed) >= 0 &&
         parsed.quantum == processLatency_.quantum &&
         parsed.rate == processLatency_.rate &&
         parsed.ns == processLatency_.ns;
}

} // namespace pipetune
