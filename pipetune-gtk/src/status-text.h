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
  /** Average native EffeTune processing time per frame. */
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

} // namespace pipetune_gtk

#endif
