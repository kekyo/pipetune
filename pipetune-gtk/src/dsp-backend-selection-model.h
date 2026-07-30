#ifndef PIPETUNE_GTK_DSP_BACKEND_SELECTION_MODEL_H
#define PIPETUNE_GTK_DSP_BACKEND_SELECTION_MODEL_H

#include "application-state.h"

#include "pipetune/dsp_backend.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pipetune_gtk {

/**
 * Describes one row in the native DSP backend drop-down.
 */
struct DspBackendChoice {
  /** Scalar compatibility or SIMD acceleration variant. */
  pipetune::DspBackendKind kind;
  /** Human-readable variant, availability, CPU requirement, and diagnostic. */
  std::string label;
  /** True after the daemon reported availability for this row. */
  bool availabilityKnown;
  /** True when the reported backend passed CPU, load, ABI, and catalog checks. */
  bool available;
};

/**
 * Describes all DSP backend-selection values required by the GTK view.
 */
struct DspBackendSelectionPresentation {
  /** Scalar followed by SIMD in stable order. */
  std::vector<DspBackendChoice> choices;
  /** Row matching the backend currently edited by the user. */
  std::size_t activeIndex;
  /** Configured, effective, fallback, and selection diagnostic summary. */
  std::string effectiveBackend;
  /** True when the edited backend is reported available. */
  bool selectedBackendAvailable;
  /** True when the connected daemon may accept a live switch. */
  bool sensitive;
};

/**
 * Maps daemon backend state into a passive GTK presentation.
 *
 * @param state Current application and daemon state.
 * @param editedBackend Backend currently represented by the GTK control.
 * @return Drop-down rows, active selection, status text, and sensitivity.
 */
DspBackendSelectionPresentation
makeDspBackendSelectionPresentation(
    const ApplicationState &state,
    pipetune::DspBackendKind editedBackend);

} // namespace pipetune_gtk

#endif
