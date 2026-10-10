/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_REVERB_H
#define PIPETUNE_ROOM_EQ_REVERB_H
#include "low_phase.h"

namespace pipetune::assets::room_eq {

/** Adopted direct/extended-window design, before optional low-frequency extension. */
struct ReverbDesign {
  RenderedFilter filter; /**< Accepted realized FIR. */
  std::vector<double> phase; /**< Accepted phase before timing and nominal FIR delay. */
  double alignment = 0; /**< Accepted advance in seconds. */
  std::optional<ReverbTarget> target; /**< Accepted reverb component for incremental low-frequency correction. */
  RoomEqDiagnostic diagnostic; /**< Effective window, agreement and guard result. */
};
/** Adds extended-window residual correction and the staged FIR energy guard.
 * @param analyses Selected normalized original responses.
 * @param timing Direct-window timing reference. @param directCorrection Direct correction phase.
 * @param minimum Minimum-phase reference. @param magnitudes Correction magnitudes.
 * @param frequencies Synthesis grid. @param config Design settings.
 * @param baseline Direct-only realized filter. @param unalignedPhase Direct-only phase.
 * @param alignment Direct-only alignment advance. @param effectiveWindow Measurement-clamped window in ms.
 * @return Accepted correction, or the exact direct-only baseline when guards reject every candidate. */
ReverbDesign designReverb(std::span<const ImpulseAnalysis> analyses, const DirectSpectrum &timing,
    std::span<const double> directCorrection, std::span<const double> minimum,
    std::span<const double> magnitudes, std::span<const double> frequencies,
    const RoomEqConfiguration &config, RenderedFilter baseline, std::vector<double> unalignedPhase,
    double alignment, double effectiveWindow);

} // namespace pipetune::assets::room_eq
#endif
