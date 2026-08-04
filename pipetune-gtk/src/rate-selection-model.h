#ifndef PIPETUNE_GTK_RATE_SELECTION_MODEL_H
#define PIPETUNE_GTK_RATE_SELECTION_MODEL_H

#include "application-state.h"

#include "pipetune/sample_rate.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace pipetune_gtk {

/**
 * Identifies what is known about one fixed rate across physical outputs.
 */
enum class DeviceRateSupport {
  /** The row is not a fixed sample rate. */
  notApplicable,
  /** PipeWire has not supplied all usable output capabilities. */
  unknown,
  /** Every reported output accepts the fixed sample rate. */
  supported,
  /** At least one output requires PipeWire resampling. */
  unsupported
};

/**
 * Describes one row in the PCM rate drop-down.
 */
struct SampleRateChoice {
  /** Maximum-following or fixed selection represented by this row. */
  pipetune::SampleRateMode mode;
  /** Fixed rate in hertz, or zero for the Max row. */
  std::uint32_t fixedRate;
  /** Human-readable row label including device support. */
  std::string label;
  /** Aggregate physical-output support represented by the row. */
  DeviceRateSupport support;
};

/**
 * Describes all PCM rate-selection values required by the GTK view.
 */
struct RateSelectionPresentation {
  /** Max followed by all user-selectable fixed rates. */
  std::vector<SampleRateChoice> choices;
  /** Row matching the policy currently edited by the user. */
  std::size_t activeRateIndex;
  /** Suggest row 0 or Force row 1. */
  std::size_t activeEnforcementIndex;
  /** Final output-specific DSP, graph, and active physical rates. */
  std::string effectiveRates;
  /** True when connected controls may be edited. */
  bool sensitive;
};

/**
 * Maps daemon capabilities and final rates into a passive GTK presentation.
 *
 * Device support is aggregated across every physical output reported by the
 * daemon. This function does not resolve or choose DSP or graph rates.
 *
 * @param state Current application and daemon state.
 * @param editedPolicy Policy currently represented by the GTK controls.
 * @return Drop-down rows, active selections, final rates, and sensitivity.
 */
RateSelectionPresentation makeRateSelectionPresentation(
    const ApplicationState &state,
    const pipetune::SampleRatePolicy &editedPolicy);

} // namespace pipetune_gtk

#endif
