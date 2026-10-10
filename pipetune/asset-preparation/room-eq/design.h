/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ROOM_EQ_DESIGN_H
#define PIPETUNE_ROOM_EQ_DESIGN_H

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace pipetune::assets {

/** Room EQ phase mode, independently of the native convolution parameters. */
enum class RoomEqPhase {
  Minimum, /**< Causal minimum-phase magnitude correction. */
  Linear, /**< Symmetric magnitude correction centered at half the tap count. */
  Correction /**< Measured phase correction with the same nominal center. */
};
/** Additional equalizer band shape. */
enum class RoomEqBandType {
  Peak, /**< Peaking equalizer. */
  LowShelf, /**< Low-frequency shelving equalizer. */
  HighShelf /**< High-frequency shelving equalizer. */
};
/** One optional RBJ equalizer band. */
struct RoomEqBand {
  RoomEqBandType type = RoomEqBandType::Peak; /**< Filter shape. */
  bool enabled = true; /**< Whether the band contributes to the target. */
  double frequency = 1000; /**< Center frequency in hertz. */
  double gain = 0; /**< Gain in decibels. */
  double q = 1; /**< Positive quality factor. */
};
/** One measured frequency-response point. */
struct RoomEqResponsePoint {
  double frequency = 0; /**< Positive frequency in hertz. */
  double decibels = 0; /**< Finite measured magnitude in decibels. */
};
/** Original impulse data for one measurement point. */
struct RoomEqImpulse {
  std::vector<float> samples; /**< Original finite float32 measurement samples. */
  std::uint32_t sampleRate = 0; /**< Original sample rate in hertz. */
  std::uint64_t onsetIndex = 0; /**< Direct-sound onset in original samples. */
  double referenceScale = 1; /**< Stored deconvolution reference scale. */
  std::uint64_t pointId = 0; /**< Original nonnegative measurement point identity. */
};
/** Memory-only measurement for one output channel. */
struct RoomEqSource {
  bool assigned = false; /**< False produces an aligned unit impulse. */
  std::vector<RoomEqResponsePoint> response; /**< Average measured frequency response. */
  std::vector<RoomEqImpulse> impulses = {}; /**< Complete point-ordered impulse set, or empty for legacy FR. */
};
/** Native design inputs corresponding to the application's design-core boundary. */
struct RoomEqConfiguration {
  std::uint32_t sampleRate = 48000; /**< Design and playback rate in hertz. */
  std::uint32_t taps = 32768; /**< Power of two, 8192 through 131072; unsupported values use 32768. */
  RoomEqPhase phase = RoomEqPhase::Linear; /**< Requested phase behavior. */
  double smoothing = 0.17; /**< Gaussian smoothing width in octaves. */
  double lowFrequency = 20; /**< Lower correction limit in hertz. */
  double highFrequency = 16000; /**< Upper correction limit in hertz. */
  double maxBoostDb = 6; /**< Maximum automatic positive correction in decibels. */
  double correctionAmount = 1; /**< Automatic correction multiplier, zero through one. */
  std::vector<RoomEqBand> eqBands; /**< Additional EQ, independent of correction amount. */
  double directWindowMs = 6; /**< Direct-response observation window, 1 through 50 ms. */
  std::optional<double> phaseLowFrequency; /**< Manual phase boundary; absent selects three cycles per window. */
  double phaseCorrectionAmount = 1; /**< Independent phase correction multiplier, zero through one. */
  double reverbAmount = 0; /**< Extended-window correction multiplier, zero through one. */
  double reverbWindowMs = 300; /**< Requested extended observation window, 20 through 1000 ms. */
  double reverbMaxFrequency = 250; /**< Upper frequency for extended-window correction. */
  double reverbSmoothing = 0.05; /**< Extended-window Gaussian smoothing width in octaves. */
  std::optional<double> phaseSmoothing; /**< Direct-phase smoothing; absent follows magnitude smoothing. */
  bool lowFrequencyPhaseExtension = false; /**< Extend phase correction below the direct-window boundary. */
  std::uint64_t referencePoint = 0; /**< Original point identity plus one; zero or missing selects consensus. */
};
/** One application-compatible correction diagnostic, independent of JSON or UI. */
struct RoomEqDiagnostic {
  std::string state; /**< Applied, reduced, disabled, or not-requested state code. */
  std::optional<double> scale; /**< Applied fraction, where defined by the application. */
  std::optional<std::string> reason; /**< Explanation code, or no limiting reason. */
  std::optional<double> effectiveWindowMs; /**< Measurement-clamped extended window. */
  std::optional<double> agreementMinimum; /**< Minimum inter-point agreement in the active band. */
  std::optional<double> residualMaximumMs; /**< Maximum residual delay when FIR length is insufficient. */
};
/** Per-channel diagnostics for the three independently controlled phase paths. */
struct RoomEqDiagnostics {
  std::vector<RoomEqDiagnostic> phaseCorrection; /**< Direct-window correction state. */
  std::vector<RoomEqDiagnostic> lowFrequencyPhaseExtension; /**< Low-frequency extension state. */
  std::vector<RoomEqDiagnostic> reverbCorrection; /**< Extended-window correction state. */
};
/** A completed design, containing only values owned by the caller. */
struct RoomEqDesign {
  std::vector<std::vector<float>> channels; /**< Channel-major finite FIR coefficients. */
  std::vector<std::string> qualityWarnings; /**< Application quality diagnostic codes. */
  std::uint32_t filterDelaySamples = 0; /**< FIR reference delay, excluding convolution scheduling. */
  bool supportsFullPhase = true; /**< Whether every assigned source has usable impulse responses. */
  RoomEqDiagnostics diagnostics; /**< Effective correction and reduction reasons per channel. */
};

/**
 * Design channel FIRs without I/O, JSON, DSP-engine, or process-global state.
 * @param configuration Design settings; finite bounded values are required.
 * @param sources One measurement or intentional unassigned entry per output.
 * @return Designed FIRs and quality/latency metadata.
 * @throws std::invalid_argument For invalid settings or incomplete assigned measurements.
 * @remarks Coefficients follow the EffeTune v2.13.0 application design contract.
 */
RoomEqDesign designRoomEq(const RoomEqConfiguration &configuration,
                         std::span<const RoomEqSource> sources);

} // namespace pipetune::assets
#endif
