#ifndef PIPETUNE_SAMPLE_RATE_H
#define PIPETUNE_SAMPLE_RATE_H

#include <cstdint>
#include <span>
#include <string_view>

namespace pipetune {

/**
 * Selects how PipeTune determines its DSP sample rate.
 */
enum class SampleRateMode {
  /** Follow the sample rate negotiated by the PipeWire graph. */
  automatic,
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
  /** Automatic graph negotiation or an explicit fixed rate. */
  SampleRateMode mode = SampleRateMode::automatic;
  /** Explicit DSP rate in hertz, or zero in automatic mode. */
  std::uint32_t fixedRate = 0;
  /** PipeWire graph-rate request behavior. */
  SampleRateEnforcement enforcement = SampleRateEnforcement::suggest;

  /** Compares complete user policy values. */
  bool operator==(const SampleRatePolicy &) const = default;
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
 * @return `automatic`, `fixed`, or an empty view for an invalid enum value.
 */
std::string_view sampleRateModeName(SampleRateMode mode) noexcept;

/**
 * Parses a configuration or protocol rate-mode name.
 *
 * @param text Exact lowercase mode name.
 * @param mode Parsed mode when successful.
 * @return True when text is `automatic` or `fixed`.
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
 * Returns the default automatic graph-negotiation policy.
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

} // namespace pipetune

#endif
