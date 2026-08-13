/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/sample_rate.h"

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

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
                   pipetune::SampleRateMode::automatic) == "automatic",
               "automatic mode name differs") &&
         check(pipetune::sampleRateModeName(
                   pipetune::SampleRateMode::fixed) == "fixed",
               "fixed mode name differs") &&
         check(pipetune::sampleRateEnforcementName(
                   pipetune::SampleRateEnforcement::suggest) == "suggest",
               "suggest enforcement name differs") &&
         check(pipetune::sampleRateEnforcementName(
                   pipetune::SampleRateEnforcement::force) == "force",
               "force enforcement name differs") &&
         check(pipetune::parseSampleRateMode("automatic", mode) &&
                   mode == pipetune::SampleRateMode::automatic,
               "automatic mode parsing differs") &&
         check(pipetune::parseSampleRateMode("fixed", mode) &&
                   mode == pipetune::SampleRateMode::fixed,
               "fixed mode parsing differs") &&
         check(!pipetune::parseSampleRateMode("max", mode),
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
  return check(defaults.mode == pipetune::SampleRateMode::automatic &&
                   defaults.fixedRate == 0 &&
                   defaults.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "default policy must be automatic and suggest") &&
         check(pipetune::sampleRatePolicyIsValid(defaults),
               "default policy must be valid") &&
         check(pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::fixed,
                    .fixedRate = 384000,
                    .enforcement =
                        pipetune::SampleRateEnforcement::force}),
               "384 kHz force policy must be valid") &&
         check(!pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::automatic,
                    .fixedRate = 48000,
                    .enforcement =
                        pipetune::SampleRateEnforcement::suggest}),
               "automatic policy must not carry a fixed rate") &&
         check(!pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::automatic,
                    .fixedRate = 0,
                    .enforcement =
                        pipetune::SampleRateEnforcement::force}),
               "automatic policy must let the graph choose its rate") &&
         check(!pipetune::sampleRatePolicyIsValid(
                   {.mode = pipetune::SampleRateMode::fixed,
                    .fixedRate = 88200,
                    .enforcement =
                        pipetune::SampleRateEnforcement::suggest}),
               "fixed policy must use a selectable rate");
}

static bool testDspRateResolution() {
  const auto fixedSuggest = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  const auto fixedForce = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  return check(
             pipetune::dspSampleRateForPolicy(fixedSuggest, 48000) ==
                 192000,
             "Suggest must keep the selected fixed DSP rate") &&
         check(pipetune::dspSampleRateForPolicy(fixedForce, 48000) ==
                   192000,
               "Force must keep the selected fixed DSP rate") &&
         check(pipetune::dspSampleRateForPolicy(
                   pipetune::defaultSampleRatePolicy(), 48000) == 48000,
               "automatic mode must follow the PipeWire graph rate");
}

int main() {
  return testSelectableRates() && testPolicyNamesAndParsing() &&
                 testPolicyValidation() && testDspRateResolution()
             ? 0
             : 1;
}
