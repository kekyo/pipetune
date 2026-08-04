#ifndef PIPETUNE_SAMPLE_RATE_H
#define PIPETUNE_SAMPLE_RATE_H

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace pipetune {

/**
 * Identifies how an output device describes supported sample rates.
 */
enum class SampleRateConstraintKind {
  /** One exact rate. */
  discrete,
  /** Every integer rate between inclusive endpoints. */
  range,
  /** Rates separated by a fixed step from the inclusive minimum. */
  step
};

/**
 * Describes one normalized output-device sample-rate constraint.
 */
struct SampleRateConstraint {
  /** Constraint representation. */
  SampleRateConstraintKind kind = SampleRateConstraintKind::discrete;
  /** Inclusive minimum rate in hertz. */
  std::uint32_t minimum = 0;
  /** Inclusive maximum rate in hertz. */
  std::uint32_t maximum = 0;
  /** Step in hertz for step constraints, otherwise zero. */
  std::uint32_t step = 0;

  /** Compares normalized constraint values. */
  bool operator==(const SampleRateConstraint &) const = default;
};

/**
 * Describes whether and how one output accepts PCM sample rates.
 */
struct SampleRateCapabilities {
  /** True after PipeWire completed a usable EnumFormat enumeration. */
  bool known = false;
  /** Normalized union of discrete, range, and step constraints. */
  std::vector<SampleRateConstraint> constraints = {};

  /** Compares capability state and normalized constraints. */
  bool operator==(const SampleRateCapabilities &) const = default;
};

/**
 * Selects how PipeTune determines its DSP sample rate.
 */
enum class SampleRateMode {
  /** Follow the highest selectable rate supported by each physical output. */
  maximum,
  /** Use one explicitly selected DSP sample rate. */
  fixed
};

/**
 * Selects how PipeTune asks PipeWire to choose the output graph rate.
 */
enum class SampleRateEnforcement {
  /** Advertise the selected rate as a preference. */
  suggest,
  /** Force the selected graph rate while PipeTune playback is active. */
  force
};

/**
 * Describes the persisted user sample-rate choice.
 */
struct SampleRatePolicy {
  /** Automatic maximum selection or an explicit fixed rate. */
  SampleRateMode mode = SampleRateMode::maximum;
  /** Explicit DSP rate in hertz, or zero in maximum mode. */
  std::uint32_t fixedRate = 0;
  /** PipeWire graph-rate request behavior. */
  SampleRateEnforcement enforcement = SampleRateEnforcement::suggest;

  /** Compares complete user policy values. */
  bool operator==(const SampleRatePolicy &) const = default;
};

/**
 * Describes the daemon-selected DSP and output graph rates.
 */
struct ResolvedSampleRates {
  /** Capture, playback media-format, and DSP rate in hertz. */
  std::uint32_t dspSampleRate = 48000;
  /** PipeWire output graph-rate hint in hertz. */
  std::uint32_t outputSampleRate = 48000;
  /** True when the output rate differs from the requested DSP policy. */
  bool fallback = false;

  /** Compares complete resolved-rate values. */
  bool operator==(const ResolvedSampleRates &) const = default;
};

/**
 * Returns the ordered DSP rates users may select.
 *
 * @return Stable ascending list in hertz.
 */
std::span<const std::uint32_t> selectableSampleRates() noexcept;

/**
 * Reports whether a rate is one of the supported user-selectable DSP rates.
 *
 * @param sampleRate Sample rate in hertz.
 * @return True for 44.1, 48, 96, 192, or 384 kHz.
 */
bool isSelectableSampleRate(std::uint32_t sampleRate) noexcept;

/**
 * Returns the configuration and protocol name of a rate mode.
 *
 * @param mode Rate-selection mode.
 * @return `max`, `fixed`, or an empty view for an invalid enum value.
 */
std::string_view sampleRateModeName(SampleRateMode mode) noexcept;

/**
 * Parses a configuration or protocol rate-mode name.
 *
 * @param text Exact lowercase mode name.
 * @param mode Parsed mode when successful.
 * @return True when text is `max` or `fixed`.
 */
bool parseSampleRateMode(std::string_view text, SampleRateMode &mode) noexcept;

/**
 * Returns the configuration and protocol name of an enforcement mode.
 *
 * @param enforcement Graph-rate enforcement behavior.
 * @return `suggest`, `force`, or an empty view for an invalid enum value.
 */
std::string_view
sampleRateEnforcementName(SampleRateEnforcement enforcement) noexcept;

/**
 * Parses a configuration or protocol enforcement name.
 *
 * @param text Exact lowercase enforcement name.
 * @param enforcement Parsed behavior when successful.
 * @return True when text is `suggest` or `force`.
 */
bool parseSampleRateEnforcement(
    std::string_view text, SampleRateEnforcement &enforcement) noexcept;

/**
 * Returns the default Max-and-suggest policy.
 *
 * @return Valid default policy.
 */
SampleRatePolicy defaultSampleRatePolicy() noexcept;

/**
 * Validates the relationship between mode, rate, and enforcement.
 *
 * @param policy Candidate policy.
 * @return True when the policy can be persisted or sent to the daemon.
 */
bool sampleRatePolicyIsValid(const SampleRatePolicy &policy) noexcept;

/**
 * Validates, orders, and deduplicates output sample-rate constraints.
 *
 * Unknown capabilities are normalized to an empty constraint list. Known
 * constraints must use positive, ascending endpoints and valid step values.
 *
 * @param capabilities Capability state to normalize in place.
 * @return True on success; false when a known constraint is malformed.
 */
bool normalizeSampleRateCapabilities(
    SampleRateCapabilities &capabilities);

/**
 * Reports whether known output capabilities accept a sample rate.
 *
 * @param capabilities Normalized or valid device capabilities.
 * @param sampleRate Candidate rate in hertz.
 * @return True when at least one known constraint contains sampleRate.
 */
bool sampleRateCapabilitiesSupport(
    const SampleRateCapabilities &capabilities,
    std::uint32_t sampleRate) noexcept;

/**
 * Resolves a user policy against one physical output's capabilities.
 *
 * Max mode retains the current rates while capabilities are unknown. Fixed
 * mode always selects its requested DSP rate and uses the same output hint
 * until capabilities become known. For a known unsupported fixed rate, the
 * output uses the greatest supported rate not above it, or the device minimum.
 *
 * @param policy Valid Max/fixed user choice.
 * @param capabilities Physical output capabilities.
 * @param currentDspSampleRate Current DSP rate, or zero for the 48 kHz initial
 * fallback.
 * @param currentOutputSampleRate Current graph hint, or zero for the 48 kHz
 * initial fallback.
 * @return Resolved rates, or nullopt for invalid inputs.
 */
std::optional<ResolvedSampleRates> resolveSampleRates(
    const SampleRatePolicy &policy,
    const SampleRateCapabilities &capabilities,
    std::uint32_t currentDspSampleRate,
    std::uint32_t currentOutputSampleRate);

} // namespace pipetune

#endif
