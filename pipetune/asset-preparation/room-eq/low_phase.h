/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_LOW_PHASE_H
#define PIPETUNE_ROOM_EQ_LOW_PHASE_H
#include "phase.h"

namespace pipetune::assets::room_eq {

/** Adopted extended-window target, used to avoid undoing longer reverb correction. */
struct ReverbTarget {
  double windowSeconds = 0; /**< Effective extended window in seconds. */
  double amountPerPhaseAmount = 0; /**< Effective reverb amount divided by direct phase amount. */
  std::vector<double> delays; /**< Adopted interval delays per unit direct phase amount. */
};
/** Guarded low-frequency extension result. */
struct LowPhaseDesign {
  RenderedFilter filter; /**< Largest safe correction, or the unchanged baseline. */
  RoomEqDiagnostic diagnostic; /**< Applied scale and limiting reason. */
};
/** @param taps Finite FIR. @return Fraction of energy within the first or last five percent. */
double edgeEnergyRatio(std::span<const float> taps);
/** @param candidate Proposed FIR. @param baseline Previously accepted FIR.
 * @return Whether the candidate satisfies the application's boundary-energy limit. */
bool phaseEnergySafe(const RenderedFilter &candidate, const RenderedFilter &baseline);
/** Extends correction below Phase Low and guards realized delay and boundary energy.
 * @param target Timing reference, with full-response guard data.
 * @param directs Selected original points. @param upperCorrection Direct-window correction phase.
 * @param alignment Alignment advance in seconds. @param magnitudes Target magnitudes.
 * @param unalignedPhase Adopted direct/reverb phase before alignment.
 * @param frequencies Synthesis grid. @param diagnosticFrequencies Residual scoring grid.
 * @param config Design settings. @param baseline Accepted design before extension.
 * @param reverb Adopted extended-window contribution, or absent.
 * @return Safely extended FIR with application-compatible diagnostics. */
LowPhaseDesign extendLowPhase(const DirectSpectrum &target, std::span<const DirectSpectrum> directs,
    std::span<const double> upperCorrection, double alignment, std::span<const double> magnitudes,
    std::span<const double> unalignedPhase, std::span<const double> frequencies,
    std::span<const double> diagnosticFrequencies, const RoomEqConfiguration &config,
    RenderedFilter baseline, const std::optional<ReverbTarget> &reverb);

} // namespace pipetune::assets::room_eq
#endif
