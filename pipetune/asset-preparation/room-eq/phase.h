/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_PHASE_H
#define PIPETUNE_ROOM_EQ_PHASE_H
#include "analysis.h"
#include "numeric.h"

namespace pipetune::assets::room_eq {

/** Direct-window spectral observation on the synthesis grid. */
struct DirectSpectrum {
  std::vector<double> magnitude; /**< Windowed magnitude. */
  std::vector<double> phase; /**< Integrated onset-relative phase. */
  std::vector<double> excessDelaySeconds; /**< Non-minimum-phase delay. */
  std::vector<std::uint8_t> valid; /**< Reliable group-delay observations. */
  std::span<const float> samples; /**< Borrowed original response, owned by the enclosing design. */
  std::size_t onsetIndex = 0; /**< Direct onset in samples. */
  std::span<const float> groupDelaySamples; /**< Borrowed full response for residual guards. */
  std::size_t groupDelayOnsetIndex = 0; /**< Onset for residual guard analysis. */
};
/** Smoothed, delay-offset-adjusted direct-phase observations. */
struct DirectPhaseAnalysis {
  std::size_t first = 0; /**< First frequency at or above the phase boundary. */
  double low = 0; /**< Lower analysis frequency. */
  double high = 0; /**< Upper analysis frequency. */
  double floor = 0; /**< Median-relative reliability floor. */
  std::vector<std::uint8_t> valid; /**< Reliable source entries, including outside the correction band. */
  std::vector<double> intervalDelays; /**< Smoothed delays in seconds, zero at invalid entries. */
};
/** Realized phase correction and the corresponding application diagnostics. */
struct PhaseDesign {
  RenderedFilter filter; /**< FIR and accuracy verification. */
  RoomEqDiagnostic phase; /**< Direct correction diagnostic. */
  RoomEqDiagnostic low; /**< Low-frequency extension diagnostic. */
  RoomEqDiagnostic reverb; /**< Extended-window correction diagnostic. */
};

/** @param config Design settings. @return Effective direct-phase lower boundary in hertz. */
double phaseLowFrequency(const RoomEqConfiguration &config);
/** @param frequency Frequency to weight. @param low Lower boundary. @param high Upper boundary.
 * @param upperLimit Nyquist guard. @return One-third-octave cosine-flanked band weight. */
double correctionWeight(double frequency, double low, double high, double upperLimit);
/** @param values Linear-grid delay values. @param valid Reliable entries. @param frequencies Grid in hertz.
 * @param smoothing Width in octaves. @return Log-grid-smoothed runs, preserving invalid gaps. */
std::vector<double> smoothSynthesisRuns(std::span<const double> values,
    std::span<const std::uint8_t> valid, std::span<const double> frequencies, double smoothing);
/** @param analyses Nonempty normalized observations. @param config Design settings.
 * @return Onset-aligned consensus response, with the application's DSP window length. */
ImpulseAnalysis alignedAverageAnalysis(std::span<const ImpulseAnalysis> analyses, const RoomEqConfiguration &config);
/** @param analysis Normalized response. @param config Design rate and window.
 * @param frequencies Synthesis grid. @param windowMs Observation duration.
 * @param taperMs Terminal taper duration. @return Windowed direct spectral observation. */
DirectSpectrum directSpectrum(const ImpulseAnalysis &analysis, const RoomEqConfiguration &config,
    std::span<const double> frequencies, double windowMs, double taperMs);
/** @param direct Spectral observation. @param frequencies Synthesis grid. @param config Design settings.
 * @param preserveAbsoluteDelay Whether to retain the in-band delay offset.
 * @return Smoothed direct-phase observations. */
DirectPhaseAnalysis directPhaseAnalysis(const DirectSpectrum &direct, std::span<const double> frequencies,
    const RoomEqConfiguration &config, bool preserveAbsoluteDelay);
/** @param directs Selected point observations. @param frequencies Synthesis grid. @param config Design settings.
 * @return Consensus direct-phase correction in radians. */
std::vector<double> consensusDirectPhase(std::span<const DirectSpectrum> directs,
    std::span<const double> frequencies, const RoomEqConfiguration &config);
/** @param direct Timing-reference observation. @param magnitudes Correction magnitudes.
 * @param referencePhase Minimum-phase reference. @param phase Candidate correction phase.
 * @param config Design settings. @param searchWindowMs Peak-search duration.
 * @return Dominant-energy alignment advance, in seconds. */
double timingAlignment(const DirectSpectrum &direct, std::span<const double> magnitudes,
    std::span<const double> referencePhase, std::span<const double> phase,
    const RoomEqConfiguration &config, double searchWindowMs);
/** @param analyses Normalized original observations. @param impulses Original identities.
 * @param magnitudes Target magnitudes. @param frequencies Synthesis grid.
 * @param diagnosticFrequencies Logarithmic diagnostic grid. @param config Design settings.
 * @return Phase-corrected FIR and effective correction diagnostics. */
PhaseDesign designPhase(std::span<const ImpulseAnalysis> analyses, std::span<const RoomEqImpulse> impulses,
    std::span<const double> magnitudes, std::span<const double> frequencies,
    std::span<const double> diagnosticFrequencies, const RoomEqConfiguration &config);

} // namespace pipetune::assets::room_eq
#endif
