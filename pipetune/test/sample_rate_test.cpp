#include "pipetune/sample_rate.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testSelectableRates() {
  constexpr auto expected =
      std::array<std::uint32_t, 5>{44100, 48000, 96000, 192000, 384000};
  const auto actual = pipetune::selectableSampleRates();
  if (!check(actual.size() == expected.size(),
             "selectable sample-rate count differs")) {
    return false;
  }
  for (auto index = std::size_t{0}; index < expected.size(); ++index) {
    if (!check(actual[index] == expected[index],
               "selectable sample-rate ordering differs") ||
        !check(pipetune::isSelectableSampleRate(expected[index]),
               "listed sample rate must be selectable")) {
      return false;
    }
  }
  return check(!pipetune::isSelectableSampleRate(32000),
               "unlisted sample rates must not be selectable") &&
         check(!pipetune::isSelectableSampleRate(88200),
               "88.2 kHz must not be selectable");
}

static bool testPolicyNamesAndParsing() {
  auto mode = pipetune::SampleRateMode::fixed;
  auto enforcement = pipetune::SampleRateEnforcement::force;
  return check(pipetune::sampleRateModeName(
                   pipetune::SampleRateMode::maximum) == "max",
               "maximum mode name differs") &&
         check(pipetune::sampleRateModeName(
                   pipetune::SampleRateMode::fixed) == "fixed",
               "fixed mode name differs") &&
         check(pipetune::sampleRateEnforcementName(
                   pipetune::SampleRateEnforcement::suggest) == "suggest",
               "suggest enforcement name differs") &&
         check(pipetune::sampleRateEnforcementName(
                   pipetune::SampleRateEnforcement::force) == "force",
               "force enforcement name differs") &&
         check(pipetune::parseSampleRateMode("max", mode) &&
                   mode == pipetune::SampleRateMode::maximum,
               "max mode parsing differs") &&
         check(pipetune::parseSampleRateMode("fixed", mode) &&
                   mode == pipetune::SampleRateMode::fixed,
               "fixed mode parsing differs") &&
         check(!pipetune::parseSampleRateMode("automatic", mode),
               "unknown rate modes must fail") &&
         check(pipetune::parseSampleRateEnforcement("suggest",
                                                    enforcement) &&
                   enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "suggest parsing differs") &&
         check(pipetune::parseSampleRateEnforcement("force",
                                                    enforcement) &&
                   enforcement == pipetune::SampleRateEnforcement::force,
               "force parsing differs") &&
         check(!pipetune::parseSampleRateEnforcement("strict",
                                                     enforcement),
               "unknown enforcement must fail");
}

static bool testPolicyValidation() {
  const auto defaults = pipetune::defaultSampleRatePolicy();
  return check(defaults.mode == pipetune::SampleRateMode::maximum &&
                   defaults.fixedRate == 0 &&
                   defaults.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "default policy must be Max and suggest") &&
         check(pipetune::sampleRatePolicyIsValid(defaults),
               "default policy must be valid") &&
         check(pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::fixed,
                    .fixedRate = 384000,
                    .enforcement =
                        pipetune::SampleRateEnforcement::force}),
               "384 kHz force policy must be valid") &&
         check(!pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::maximum,
                    .fixedRate = 48000,
                    .enforcement =
                        pipetune::SampleRateEnforcement::suggest}),
               "maximum policy must not carry a fixed rate") &&
         check(!pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::fixed,
                    .fixedRate = 88200,
                    .enforcement =
                        pipetune::SampleRateEnforcement::suggest}),
               "fixed policy must use a selectable rate");
}

static bool testCapabilityNormalizationAndSupport() {
  auto capabilities = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::step,
            .minimum = 32000,
            .maximum = 96000,
            .step = 16000},
           {.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 192000,
            .maximum = 192000,
            .step = 0},
           {.kind = pipetune::SampleRateConstraintKind::range,
            .minimum = 44100,
            .maximum = 48000,
            .step = 0},
           {.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 192000,
            .maximum = 192000,
            .step = 0}}};
  if (!check(pipetune::normalizeSampleRateCapabilities(capabilities),
             "valid capabilities must normalize") ||
      !check(capabilities.constraints.size() == 3,
             "normalization must remove duplicate constraints") ||
      !check(capabilities.constraints[0].minimum == 32000 &&
                 capabilities.constraints[1].minimum == 44100 &&
                 capabilities.constraints[2].minimum == 192000,
             "constraints must be ordered by minimum rate")) {
    return false;
  }

  return check(pipetune::sampleRateCapabilitiesSupport(capabilities,
                                                       32000),
               "step minimum must be supported") &&
         check(pipetune::sampleRateCapabilitiesSupport(capabilities,
                                                       48000),
               "step/range endpoint must be supported") &&
         check(!pipetune::sampleRateCapabilitiesSupport(capabilities,
                                                        50000),
               "a value outside every constraint must be unsupported") &&
         check(pipetune::sampleRateCapabilitiesSupport(capabilities,
                                                       192000),
               "discrete rate must be supported") &&
         check(!pipetune::sampleRateCapabilitiesSupport(
                   {.known = false, .constraints = {}}, 48000),
               "unknown capabilities must not claim support");
}

static bool testCapabilityValidation() {
  auto unknownWithConstraint = pipetune::SampleRateCapabilities{
      .known = false,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::discrete,
            .minimum = 48000,
            .maximum = 48000,
            .step = 0}}};
  auto invalidRange = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::range,
            .minimum = 96000,
            .maximum = 48000,
            .step = 0}}};
  auto invalidStep = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::step,
            .minimum = 44100,
            .maximum = 192000,
            .step = 0}}};
  return check(
             pipetune::normalizeSampleRateCapabilities(unknownWithConstraint) &&
                 unknownWithConstraint.constraints.empty(),
             "unknown capabilities must normalize to no constraints") &&
         check(!pipetune::normalizeSampleRateCapabilities(invalidRange),
               "descending ranges must be rejected") &&
         check(!pipetune::normalizeSampleRateCapabilities(invalidStep),
               "step constraints must require a positive step");
}

static bool testMaximumRateResolution() {
  const auto known = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::range,
            .minimum = 32000,
            .maximum = 192000,
            .step = 0}}};
  const auto maximum = pipetune::resolveSampleRates(
      pipetune::defaultSampleRatePolicy(), known, 48000, 48000);
  const auto unknown = pipetune::resolveSampleRates(
      pipetune::defaultSampleRatePolicy(),
      {.known = false, .constraints = {}}, 96000, 88200);
  const auto noSelectable = pipetune::resolveSampleRates(
      pipetune::defaultSampleRatePolicy(),
      {.known = true,
       .constraints =
           {{.kind = pipetune::SampleRateConstraintKind::discrete,
             .minimum = 32000,
             .maximum = 32000,
             .step = 0}}},
      192000, 192000);

  return check(maximum.has_value() &&
                   *maximum == pipetune::ResolvedSampleRates{
                                   .dspSampleRate = 192000,
                                   .outputSampleRate = 192000,
                                   .fallback = false},
               "Max must choose the highest supported selectable rate") &&
         check(unknown.has_value() &&
                   *unknown == pipetune::ResolvedSampleRates{
                                   .dspSampleRate = 96000,
                                   .outputSampleRate = 88200,
                                   .fallback = false},
               "unknown Max capabilities must retain current rates") &&
         check(noSelectable.has_value() &&
                   *noSelectable ==
                       pipetune::ResolvedSampleRates{
                           .dspSampleRate = 48000,
                           .outputSampleRate = 32000,
                           .fallback = true},
               "Max without a selectable device rate must use 48 kHz DSP "
               "and a device fallback");
}

static bool testFixedRateResolution() {
  const auto fixed192 = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  const auto fixed441 = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 44100,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  const auto stepped = pipetune::SampleRateCapabilities{
      .known = true,
      .constraints =
          {{.kind = pipetune::SampleRateConstraintKind::step,
            .minimum = 48000,
            .maximum = 144000,
            .step = 48000}}};
  const auto belowMaximum =
      pipetune::resolveSampleRates(fixed192, stepped, 48000, 48000);
  const auto belowMinimum =
      pipetune::resolveSampleRates(fixed441, stepped, 48000, 48000);
  const auto unknown = pipetune::resolveSampleRates(
      fixed192, {.known = false, .constraints = {}}, 48000, 48000);

  return check(belowMaximum.has_value() &&
                   *belowMaximum ==
                       pipetune::ResolvedSampleRates{
                           .dspSampleRate = 192000,
                           .outputSampleRate = 144000,
                           .fallback = true},
               "unsupported fixed R must choose the greatest H below R") &&
         check(belowMinimum.has_value() &&
                   *belowMinimum ==
                       pipetune::ResolvedSampleRates{
                           .dspSampleRate = 44100,
                           .outputSampleRate = 48000,
                           .fallback = true},
               "fixed R below device minimum must choose that minimum") &&
         check(unknown.has_value() &&
                   *unknown ==
                       pipetune::ResolvedSampleRates{
                           .dspSampleRate = 192000,
                           .outputSampleRate = 192000,
                           .fallback = false},
               "unknown fixed capabilities must still apply the requested R") &&
         check(!pipetune::resolveSampleRates(
                    {.mode = pipetune::SampleRateMode::fixed,
                     .fixedRate = 88200,
                     .enforcement =
                         pipetune::SampleRateEnforcement::suggest},
                    stepped, 48000, 48000)
                    .has_value(),
               "invalid policies must not resolve");
}

int main() {
  return testSelectableRates() && testPolicyNamesAndParsing() &&
                 testPolicyValidation() &&
                 testCapabilityNormalizationAndSupport() &&
                 testCapabilityValidation() &&
                 testMaximumRateResolution() &&
                 testFixedRateResolution()
             ? 0
             : 1;
}
