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

static std::optional<std::uint32_t> greatestSupportedNotAbove(
    const SampleRateCapabilities &capabilities,
    std::uint32_t limit) noexcept {
  auto selected = std::optional<std::uint32_t>{};
  for (const auto &constraint : capabilities.constraints) {
    if (!constraintIsValid(constraint) || limit < constraint.minimum) {
      continue;
    }
    const auto upper = std::min(limit, constraint.maximum);
    auto candidate = upper;
    if (constraint.kind == SampleRateConstraintKind::step) {
      candidate =
          constraint.minimum +
          ((upper - constraint.minimum) / constraint.step) * constraint.step;
    }
    if (!selected.has_value() || candidate > *selected) {
      selected = candidate;
    }
  }
  return selected;
}

static std::optional<std::uint32_t> minimumSupported(
    const SampleRateCapabilities &capabilities) noexcept {
  auto selected = std::optional<std::uint32_t>{};
  for (const auto &constraint : capabilities.constraints) {
    if (!constraintIsValid(constraint)) {
      continue;
    }
    if (!selected.has_value() || constraint.minimum < *selected) {
      selected = constraint.minimum;
    }
  }
  return selected;
}

static std::uint32_t fallbackOutputRate(
    const SampleRateCapabilities &capabilities,
    std::uint32_t dspSampleRate) noexcept {
  const auto below =
      greatestSupportedNotAbove(capabilities, dspSampleRate);
  if (below.has_value()) {
    return *below;
  }
  const auto minimum = minimumSupported(capabilities);
  return minimum.value_or(dspSampleRate);
}

std::optional<ResolvedSampleRates> resolveSampleRates(
    const SampleRatePolicy &policy,
    const SampleRateCapabilities &capabilities,
    std::uint32_t currentDspSampleRate,
    std::uint32_t currentOutputSampleRate) {
  if (!sampleRatePolicyIsValid(policy)) {
    return std::nullopt;
  }
  auto normalized = capabilities;
  if (!normalizeSampleRateCapabilities(normalized)) {
    return std::nullopt;
  }

  if (policy.mode == SampleRateMode::fixed) {
    if (!normalized.known ||
        sampleRateCapabilitiesSupport(normalized, policy.fixedRate)) {
      return ResolvedSampleRates{
          .dspSampleRate = policy.fixedRate,
          .outputSampleRate = policy.fixedRate,
          .fallback = false};
    }
    return ResolvedSampleRates{
        .dspSampleRate = policy.fixedRate,
        .outputSampleRate =
            fallbackOutputRate(normalized, policy.fixedRate),
        .fallback = true};
  }

  if (!normalized.known) {
    return ResolvedSampleRates{
        .dspSampleRate =
            currentDspSampleRate == 0 ? 48000 : currentDspSampleRate,
        .outputSampleRate =
            currentOutputSampleRate == 0 ? 48000 : currentOutputSampleRate,
        .fallback = false};
  }
  const auto selectable = selectableSampleRates();
  for (auto iterator = selectable.rbegin(); iterator != selectable.rend();
       ++iterator) {
    if (sampleRateCapabilitiesSupport(normalized, *iterator)) {
      return ResolvedSampleRates{
          .dspSampleRate = *iterator,
          .outputSampleRate = *iterator,
          .fallback = false};
    }
  }
  return ResolvedSampleRates{
      .dspSampleRate = 48000,
      .outputSampleRate = fallbackOutputRate(normalized, 48000),
      .fallback = true};
}

} // namespace pipetune
