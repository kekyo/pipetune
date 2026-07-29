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
  /** Follow the highest selectable rate supported by the selected output. */
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

} // namespace pipetune

#endif
