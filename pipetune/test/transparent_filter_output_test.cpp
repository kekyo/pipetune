#include "transparent_filter_output.h"

#include <spa/param/audio/raw.h>

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

static pipetune::TransparentFilterOutputCandidate makePhysicalOutput(
    std::uint32_t id, std::string_view name, std::uint32_t channelCount,
    std::vector<std::uint32_t> positions) {
  return {
      .id = id,
      .mediaClass = "Audio/Sink",
      .nodeName = std::string(name),
      .description = "Test output",
      .deviceApi = "alsa",
      .hasDevice = true,
      .virtualNode = false,
      .networkNode = false,
      .encodedOnly = false,
      .smartFilter = false,
      .channelCount = channelCount,
      .channelPositions = std::move(positions),
      .sampleRateCapabilities = {},
      .activeSampleRate = 0,
  };
}

static bool testPhysicalOutputEligibility() {
  const auto policy = pipetune::defaultSampleRatePolicy();
  const auto stereo = pipetune::evaluateTransparentFilterOutput(
      makePhysicalOutput(41, "alsa_output.usb-device", 2,
                         {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR}),
      policy);
  if (!check(stereo.output.has_value(), stereo.error) ||
      !check(stereo.rejection ==
                 pipetune::TransparentFilterOutputRejection::none,
             "physical ALSA output must be accepted")) {
    return false;
  }
  if (!check(stereo.output->nodeName == "alsa_output.usb-device" &&
                 stereo.output->filterNodeName == "pipetune.filter.41" &&
                 stereo.output->filterLinkGroup ==
                     "pipetune.filter.41.link-group",
             "accepted output must have stable per-node filter identities") ||
      !check(stereo.output->channelPositions ==
                 std::vector<std::uint32_t>{SPA_AUDIO_CHANNEL_FL,
                                            SPA_AUDIO_CHANNEL_FR},
             "accepted output must retain its exact channel layout") ||
      !check(stereo.output->rates ==
                 pipetune::ResolvedSampleRates{.dspSampleRate = 48000,
                                               .outputSampleRate = 48000,
                                               .fallback = false},
             "unknown device rates must initially use 48 kHz")) {
    return false;
  }

  auto bluetooth = makePhysicalOutput(
      42, "bluez_output.headphones", 2,
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  bluetooth.deviceApi = "bluez5";
  return check(
      pipetune::evaluateTransparentFilterOutput(bluetooth, policy)
          .output.has_value(),
      "Bluetooth physical outputs must be accepted");
}

static bool testPerOutputRatesAndTracking() {
  auto stereo = makePhysicalOutput(
      7, "alsa_output.stereo", 2,
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  stereo.sampleRateCapabilities = {
      .known = true,
      .constraints = {{.kind = pipetune::SampleRateConstraintKind::discrete,
                       .minimum = 48000,
                       .maximum = 48000,
                       .step = 0}}};
  auto surround = makePhysicalOutput(
      8, "alsa_output.surround", 4,
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_RL,
       SPA_AUDIO_CHANNEL_RR});
  surround.sampleRateCapabilities = {
      .known = true,
      .constraints = {{.kind = pipetune::SampleRateConstraintKind::range,
                       .minimum = 44100,
                       .maximum = 192000,
                       .step = 0}}};

  auto tracker = pipetune::TransparentFilterOutputTracker(
      pipetune::defaultSampleRatePolicy());
  auto incomplete = makePhysicalOutput(6, "alsa_output.incomplete", 0, {});
  if (!check(tracker.update(incomplete),
             "an unsupported output must change the tracker") ||
      !check(tracker.rejectedOutputs().size() == 1 &&
                 tracker.rejectedOutputs()[0].id == 6 &&
                 tracker.rejectedOutputs()[0].rejection ==
                     pipetune::TransparentFilterOutputRejection::unsupportedLayout &&
                 !tracker.rejectedOutputs()[0].error.empty(),
             "an unsupported output must retain its direct-routing reason")) {
    return false;
  }
  incomplete.channelCount = 2;
  incomplete.channelPositions = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
  if (!check(tracker.update(incomplete),
             "a newly eligible output must change the tracker") ||
      !check(tracker.rejectedOutputs().empty(),
             "an eligible output must no longer report a rejection") ||
      !check(tracker.remove(6),
             "the diagnostic transition output must be removable")) {
    return false;
  }
  if (!check(tracker.update(stereo),
             "first physical output must change the tracker") ||
      !check(tracker.update(surround),
             "second physical output must change the tracker") ||
      !check(tracker.outputs().size() == 2,
             "all physical outputs must be tracked simultaneously") ||
      !check(tracker.outputs()[0].id == 7 &&
                 tracker.outputs()[0].rates.dspSampleRate == 48000 &&
                 tracker.outputs()[1].id == 8 &&
                 tracker.outputs()[1].rates.dspSampleRate == 192000,
             "maximum rate must be resolved independently per output") ||
      !check(!tracker.update(stereo),
             "an unchanged output must not restart its runtime")) {
    return false;
  }

  const auto fixed = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 96000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  if (!check(tracker.setRatePolicy(fixed),
             "changing rate policy must update per-output formats") ||
      !check(tracker.outputs()[0].rates ==
                 pipetune::ResolvedSampleRates{.dspSampleRate = 96000,
                                               .outputSampleRate = 48000,
                                               .fallback = true},
             "unsupported fixed rate must preserve DSP rate and fall back "
             "only at output") ||
      !check(tracker.outputs()[1].rates ==
                 pipetune::ResolvedSampleRates{.dspSampleRate = 96000,
                                               .outputSampleRate = 96000,
                                               .fallback = false},
             "supported fixed rate must be retained for that output") ||
      !check(tracker.remove(7), "removing a tracked output must change state") ||
      !check(tracker.outputs().size() == 1 && tracker.outputs()[0].id == 8,
             "only the removed output runtime must disappear") ||
      !check(!tracker.remove(7),
             "removing an unknown output must not change state")) {
    return false;
  }
  return true;
}

static bool rejected(
    pipetune::TransparentFilterOutputCandidate candidate,
    pipetune::TransparentFilterOutputRejection expected,
    std::string_view message) {
  const auto evaluated = pipetune::evaluateTransparentFilterOutput(
      candidate, pipetune::defaultSampleRatePolicy());
  return check(!evaluated.output.has_value() &&
                   evaluated.rejection == expected &&
                   !evaluated.error.empty(),
               message);
}

static bool testUnsupportedOutputsBypass() {
  auto candidate = makePhysicalOutput(
      10, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  candidate.virtualNode = true;
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::virtualNode,
                "virtual outputs must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      11, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  candidate.hasDevice = false;
  candidate.deviceApi.clear();
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::notDeviceBacked,
                "null sinks must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      12, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  candidate.networkNode = true;
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::networkNode,
                "network sinks must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      13, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  candidate.encodedOnly = true;
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::encodedOnly,
                "encoded-only sinks must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      14, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR});
  candidate.smartFilter = true;
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::smartFilter,
                "existing smart filters must not be treated as physical outputs")) {
    return false;
  }

  candidate = makePhysicalOutput(
      15, "test", 9,
      {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FC,
       SPA_AUDIO_CHANNEL_LFE, SPA_AUDIO_CHANNEL_RL, SPA_AUDIO_CHANNEL_RR,
       SPA_AUDIO_CHANNEL_SL, SPA_AUDIO_CHANNEL_SR, SPA_AUDIO_CHANNEL_RC});
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::unsupportedLayout,
                "outputs above eight channels must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      16, "test", 2, {SPA_AUDIO_CHANNEL_FL});
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::unsupportedLayout,
                "incomplete layouts must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      17, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FL});
  if (!rejected(candidate,
                pipetune::TransparentFilterOutputRejection::unsupportedLayout,
                "duplicate channel positions must bypass PipeTune")) {
    return false;
  }

  candidate = makePhysicalOutput(
      18, "test", 2, {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_UNKNOWN});
  return rejected(candidate,
                  pipetune::TransparentFilterOutputRejection::unsupportedLayout,
                  "unknown channel positions must bypass PipeTune");
}

int main() {
  return testPhysicalOutputEligibility() &&
                 testPerOutputRatesAndTracking() &&
                 testUnsupportedOutputsBypass()
             ? 0
             : 1;
}
