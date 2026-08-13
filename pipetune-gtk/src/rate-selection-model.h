/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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
 * Describes one row in the PCM rate drop-down.
 */
struct SampleRateChoice {
  /** Automatic or fixed selection represented by this row. */
  pipetune::SampleRateMode mode;
  /** Fixed rate in hertz, or zero for automatic negotiation. */
  std::uint32_t fixedRate;
  /** Human-readable row label. */
  std::string label;
};

/**
 * Describes all PCM rate-selection values required by the GTK view.
 */
struct RateSelectionPresentation {
  /** Automatic followed by all user-selectable fixed rates. */
  std::vector<SampleRateChoice> choices;
  /** Row matching the policy currently edited by the user. */
  std::size_t activeRateIndex;
  /** Suggest row 0 or Force row 1. */
  std::size_t activeEnforcementIndex;
  /** True when fixed-rate enforcement may be edited. */
  bool enforcementSensitive;
  /** Active DSP and negotiated graph rates. */
  std::string effectiveRates;
  /** True when connected controls may be edited. */
  bool sensitive;
};

/**
 * Maps the configured policy and negotiated rates into a GTK presentation.
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
