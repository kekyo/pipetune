#include "pipetune/sample_rate.h"

#include <array>
#include <algorithm>

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
  case SampleRateMode::maximum:
    return "max";
  case SampleRateMode::fixed:
    return "fixed";
  }
  return {};
}

bool parseSampleRateMode(std::string_view text,
                         SampleRateMode &mode) noexcept {
  if (text == "max") {
    mode = SampleRateMode::maximum;
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
  return {.mode = SampleRateMode::maximum,
          .fixedRate = 0,
          .enforcement = SampleRateEnforcement::suggest};
}

bool sampleRatePolicyIsValid(const SampleRatePolicy &policy) noexcept {
  if (sampleRateEnforcementName(policy.enforcement).empty()) {
    return false;
  }
  switch (policy.mode) {
  case SampleRateMode::maximum:
    return policy.fixedRate == 0;
  case SampleRateMode::fixed:
    return isSelectableSampleRate(policy.fixedRate);
  }
  return false;
}

} // namespace pipetune
