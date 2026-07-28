#ifndef PIPETUNE_GTK_OUTPUT_SELECTION_MODEL_H
#define PIPETUNE_GTK_OUTPUT_SELECTION_MODEL_H

#include "application-state.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pipetune_gtk {

/**
 * Describes one row in the output-preference drop-down.
 */
struct OutputDeviceChoice {
  /** True for the system-default row that clears an explicit preference. */
  bool clearPreference;
  /** PipeWire node.name, or empty for the system-default row. */
  std::string target;
  /** Human-readable row label. */
  std::string label;
  /** True for a retained preference that is currently absent. */
  bool unavailable;
};

/**
 * Describes all output-selection values required by the GTK view.
 */
struct OutputSelectionPresentation {
  /** System-default row, available outputs, and any absent preference. */
  std::vector<OutputDeviceChoice> choices;
  /** Row matching the engine-owned explicit preference. */
  std::size_t activeIndex;
  /** Human-readable engine-selected effective output. */
  std::string effectiveOutput;
  /** Human-readable engine-provided selection reason. */
  std::string reason;
  /** True when the user may request a different preference. */
  bool sensitive;
};

/**
 * Maps engine-owned output status into a passive GTK presentation.
 *
 * This function does not choose a playback target. It only presents the
 * preference, effective target, reason, and candidates reported by the daemon.
 *
 * @param state Current application and daemon state.
 * @return Drop-down rows, active preference, effective output, and sensitivity.
 */
OutputSelectionPresentation
makeOutputSelectionPresentation(const ApplicationState &state);

} // namespace pipetune_gtk

#endif
