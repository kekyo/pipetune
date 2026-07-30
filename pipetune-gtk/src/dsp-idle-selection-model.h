#ifndef PIPETUNE_GTK_DSP_IDLE_SELECTION_MODEL_H
#define PIPETUNE_GTK_DSP_IDLE_SELECTION_MODEL_H

#include "application-state.h"

#include "pipetune/dsp_idle.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pipetune_gtk {

/**
 * Describes one row in the DSP idle policy drop-down.
 */
struct DspIdleChoice {
  /** Conservative tail threshold or exact-zero output policy. */
  pipetune::DspIdlePolicy policy;
  /** Human-readable policy and wake/sleep behavior. */
  std::string label;
};

/**
 * Describes all DSP idle values required by the GTK view.
 */
struct DspIdleSelectionPresentation {
  /** Conservative and exact-zero policy rows. */
  std::vector<DspIdleChoice> choices;
  /** Row matching the policy edited by the user. */
  std::size_t activeIndex;
  /** Runtime controller state, counters, and PipeWire idling summary. */
  std::string runtimeStatus;
  /** True when a connected daemon may accept a live policy change. */
  bool sensitive;
};

/**
 * Maps daemon DSP idle state into a passive GTK presentation.
 *
 * @param state Current application and daemon state.
 * @param editedPolicy Policy currently represented by the GTK control.
 * @return Drop-down rows, active selection, status text, and sensitivity.
 */
DspIdleSelectionPresentation makeDspIdleSelectionPresentation(
    const ApplicationState &state,
    pipetune::DspIdlePolicy editedPolicy);

} // namespace pipetune_gtk

#endif
