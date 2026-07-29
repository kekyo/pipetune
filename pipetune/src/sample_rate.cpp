#include "pipetune/sample_rate.h"

#include <array>
#include <algorithm>
#include <tuple>

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

static bool constraintIsValid(
    const SampleRateConstraint &constraint) noexcept {
  if (constraint.minimum == 0 ||
      constraint.maximum < constraint.minimum) {
    return false;
  }
  switch (constraint.kind) {
  case SampleRateConstraintKind::discrete:
    return constraint.minimum == constraint.maximum &&
           constraint.step == 0;
  case SampleRateConstraintKind::range:
    return constraint.step == 0;
  case SampleRateConstraintKind::step:
    return constraint.step != 0;
  }
  return false;
}

bool normalizeSampleRateCapabilities(
    SampleRateCapabilities &capabilities) {
  if (!capabilities.known) {
    capabilities.constraints.clear();
    return true;
  }
  for (const auto &constraint : capabilities.constraints) {
    if (!constraintIsValid(constraint)) {
      return false;
    }
  }
  for (auto &constraint : capabilities.constraints) {
    if (constraint.minimum == constraint.maximum) {
      constraint.kind = SampleRateConstraintKind::discrete;
      constraint.step = 0;
    }
  }
  std::sort(
      capabilities.constraints.begin(), capabilities.constraints.end(),
      [](const SampleRateConstraint &left,
         const SampleRateConstraint &right) {
        return std::tie(left.minimum, left.maximum, left.kind, left.step) <
               std::tie(right.minimum, right.maximum, right.kind, right.step);
      });
  capabilities.constraints.erase(
      std::unique(capabilities.constraints.begin(),
                  capabilities.constraints.end()),
      capabilities.constraints.end());
  return true;
}

bool sampleRateCapabilitiesSupport(
    const SampleRateCapabilities &capabilities,
    std::uint32_t sampleRate) noexcept {
  if (!capabilities.known || sampleRate == 0) {
    return false;
  }
  for (const auto &constraint : capabilities.constraints) {
    if (!constraintIsValid(constraint) ||
        sampleRate < constraint.minimum ||
        sampleRate > constraint.maximum) {
      continue;
    }
    if (constraint.kind != SampleRateConstraintKind::step ||
        (sampleRate - constraint.minimum) % constraint.step == 0) {
      return true;
    }
  }
  return false;
}

} // namespace pipetune
