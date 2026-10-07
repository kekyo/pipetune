/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GTK_STATUS_TEXT_H
#define PIPETUNE_GTK_STATUS_TEXT_H

#include "application-state.h"

#include <cstdint>
#include <string>

namespace pipetune_gtk {

/**
 * Contains text for the four PipeWire input status rows.
 */
struct InputStatusText {
  /** Measured input frame rate. */
  std::string frameRate;
  /** Local time and relative age of the latest input. */
  std::string lastReceived;
  /** Measured uncompressed PCM data rate. */
  std::string pcmDataRate;
  /** Negotiated sample format, rate, and channel count. */
  std::string streamFormat;
};

/**
 * Contains text for periodically published runtime measurements.
 */
struct RuntimeStatusText {
  /**
   * Average native EffeTune processing time and input-frame budget load.
   */
  std::string dspProcessingTime;
  /** Audio bridge and DSP error counters. */
  std::string counters;
};

/**
 * Formats the current PipeWire input status for GTK labels.
 *
 * @param state Current display-independent application state.
 * @param currentUnixMilliseconds Current Unix wall time in milliseconds.
 * @return Text for all four input status rows.
 */
InputStatusText inputStatusText(const ApplicationState &state,
                                std::uint64_t currentUnixMilliseconds);

/**
 * Formats the latest periodically published runtime measurements.
 *
 * @param state Current display-independent application state.
 * @return Text for DSP timing and runtime counters.
 */
RuntimeStatusText runtimeStatusText(const ApplicationState &state);

/**
 * Formats reported physical controls independently of the OS master volume.
 * @param volume Report for the current connection, or null when unavailable.
 * @return Mute, scalar gain, and the range of effective channel gains in dB.
 * @remarks Channel gains are summarized without assuming their report order
 * matches the saved routing. Unknown controls never imply unity or unmuted.
 */
std::string outputVolumeText(const pipetune::OutputVolumeState *volume);

/**
 * Formats audio path activity and a clearly identified compensation estimate.
 * @param timing Report for the current routing and connection, or null.
 * @return Activity and estimated compensation in milliseconds, or unknown.
 * @remarks An active path does not imply that its timing is available. The
 * estimate does not measure the internal buffer or acoustic arrival time.
 */
std::string outputTimingText(const pipetune::OutputTimingState *timing);

} // namespace pipetune_gtk

#endif
