/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_LATENCY_H
#define PIPETUNE_PIPEWIRE_LATENCY_H

#include <spa/param/latency.h>
#include <spa/pod/pod.h>

#include <array>
#include <cstdint>

namespace pipetune {

/** Result of converting all internal latency into PipeWire stream frames. */
struct PipeWireProcessLatencyCalculation {
  /** Aggregate processing delay in negotiated stream frames. */
  std::uint32_t frames;
  /** True when the rates and resulting frame count are representable. */
  bool valid;
};

/**
 * Converts DSP and sample-rate bridge latency into PipeWire stream frames.
 *
 * DSP frames are rounded up so the graph is never told a latency shorter than
 * the actual processing delay. Bridge frames are already measured in the
 * negotiated stream domain.
 *
 * @param dspLatencyFrames Aggregate native DSP delay in DSP-rate frames.
 * @param dspSampleRate Native DSP rate in hertz.
 * @param bridgeLatencyFrames Measured bridge delay in stream-rate frames.
 * @param streamSampleRate Negotiated PipeWire stream rate in hertz.
 * @return Aggregate stream-frame latency and validity.
 */
PipeWireProcessLatencyCalculation calculatePipeWireProcessLatency(
    std::uint32_t dspLatencyFrames, std::uint32_t dspSampleRate,
    std::uint32_t bridgeLatencyFrames,
    std::uint32_t streamSampleRate) noexcept;

/** Result of accepting a propagated PipeWire port Latency parameter. */
struct PipeWirePortLatencyUpdate {
  /** True when the parameter was a valid Latency object. */
  bool valid;
  /** Direction whose propagated value changed when valid is true. */
  spa_direction direction;
};

/**
 * Tracks bidirectional port latency and applies PipeTune ProcessLatency.
 *
 * Output latency received at the filter input is later published at the
 * filter output. Input latency received at the filter output is later
 * published at the filter input, with PipeTune's processing delay added in
 * both cases.
 */
class PipeWireLatencyState final {
public:
  /** Creates zero input and output latency with zero processing delay. */
  PipeWireLatencyState() noexcept;

  /**
   * Parses and stores one propagated port Latency parameter.
   *
   * @param parameter PipeWire SPA_PARAM_Latency object.
   * @return Parse status and the stored latency direction.
   */
  PipeWirePortLatencyUpdate
  updatePortLatency(const spa_pod *parameter) noexcept;

  /**
   * Replaces PipeTune's processing delay.
   *
   * Only rate-relative frames are populated because PipeWire adds the
   * quantum, rate, and nanosecond components independently.
   *
   * @param frames Processing delay in negotiated stream frames.
   */
  void setProcessLatencyFrames(std::uint32_t frames) noexcept;

  /** Returns the ProcessLatency value owned by PipeTune. */
  spa_process_latency_info processLatency() const noexcept;

  /**
   * Returns one stored port latency with ProcessLatency added.
   *
   * @param direction Input for upstream propagation or output for downstream.
   * @return Propagated Latency value, or zero output latency for an invalid
   * direction.
   */
  spa_latency_info propagatedLatency(spa_direction direction) const noexcept;

  /**
   * Checks whether a ProcessLatency parameter matches the owned value.
   *
   * @param parameter SPA_PARAM_ProcessLatency object, or null.
   * @return True only for a valid object equal to processLatency().
   */
  bool processLatencyMatches(const spa_pod *parameter) const noexcept;

private:
  std::array<spa_latency_info, 2> latencies_;
  spa_process_latency_info processLatency_;
};

} // namespace pipetune

#endif
