#include "pipetune/sample_rate.h"

#include <algorithm>
#include <array>

namespace pipetune {

constexpr auto kSelectableSampleRates =
    std::array<std::uint32_t, 5>{44100, 48000, 96000, 192000, 384000};

std::span<const std::uint32_t> selectableSampleRates() noexcept {
  return kSelectableSampleRates;
}

bool isSelectableSampleRate(std::uint32_t sampleRate) noexcept {
  return std::ranges::find(kSelectableSampleRates, sampleRate) !=
         kSelectableSampleRates.end();
}

std::string_view sampleRateModeName(SampleRateMode mode) noexcept {
  switch (mode) {
  case SampleRateMode::automatic:
    return "automatic";
  case SampleRateMode::fixed:
    return "fixed";
  }
  return {};
}

bool parseSampleRateMode(std::string_view text,
                         SampleRateMode &mode) noexcept {
  if (text == "automatic") {
    mode = SampleRateMode::automatic;
    return true;
  }
  if (text == "fixed") {
    mode = SampleRateMode::fixed;
    return true;
  }
  return false;
}

std::string_view
sampleRateEnforcementName(SampleRateEnforcement enforcement) noexcept {
  switch (enforcement) {
  case SampleRateEnforcement::suggest:
    return "suggest";
  case SampleRateEnforcement::force:
    return "force";
  }
  return {};
}

bool parseSampleRateEnforcement(
    std::string_view text, SampleRateEnforcement &enforcement) noexcept {
  if (text == "suggest") {
    enforcement = SampleRateEnforcement::suggest;
    return true;
  }
  if (text == "force") {
    enforcement = SampleRateEnforcement::force;
    return true;
  }
  return false;
}

SampleRatePolicy defaultSampleRatePolicy() noexcept {
  return {.mode = SampleRateMode::automatic,
          .fixedRate = 0,
          .enforcement = SampleRateEnforcement::suggest};
}

bool sampleRatePolicyIsValid(const SampleRatePolicy &policy) noexcept {
  if (sampleRateEnforcementName(policy.enforcement).empty()) {
    return false;
  }
  switch (policy.mode) {
  case SampleRateMode::automatic:
    return policy.fixedRate == 0 &&
           policy.enforcement == SampleRateEnforcement::suggest;
  case SampleRateMode::fixed:
    return isSelectableSampleRate(policy.fixedRate);
  }
  return false;
}

std::uint32_t
dspSampleRateForPolicy(const SampleRatePolicy &policy,
                       std::uint32_t negotiatedSampleRate) noexcept {
  return policy.mode == SampleRateMode::fixed ? policy.fixedRate
                                               : negotiatedSampleRate;
}

} // namespace pipetune
