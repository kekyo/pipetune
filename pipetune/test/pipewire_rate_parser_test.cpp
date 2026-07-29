#include "pipewire_rate_parser.h"

#include <spa/param/format.h>
#include <spa/pod/builder.h>

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

static bool testDiscreteAndEnumRates() {
  auto constraints = std::vector<pipetune::SampleRateConstraint>{};

  auto discreteStorage = std::array<std::uint8_t, 512>{};
  auto discreteBuilder = spa_pod_builder{};
  spa_pod_builder_init(&discreteBuilder, discreteStorage.data(),
                       discreteStorage.size());
  auto discreteFrame = spa_pod_frame{};
  spa_pod_builder_push_object(&discreteBuilder, &discreteFrame,
                              SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&discreteBuilder,
                      SPA_FORMAT_mediaType,
                      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
                      SPA_FORMAT_mediaSubtype,
                      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                      SPA_FORMAT_AUDIO_rate, SPA_POD_Int(48000), 0);
  const auto *discrete = static_cast<const spa_pod *>(
      spa_pod_builder_pop(&discreteBuilder, &discreteFrame));

  auto enumStorage = std::array<std::uint8_t, 512>{};
  auto enumBuilder = spa_pod_builder{};
  spa_pod_builder_init(&enumBuilder, enumStorage.data(), enumStorage.size());
  auto enumFrame = spa_pod_frame{};
  spa_pod_builder_push_object(&enumBuilder, &enumFrame,
                              SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      &enumBuilder, SPA_FORMAT_mediaType,
      SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_rate,
      SPA_POD_CHOICE_ENUM_Int(4, 48000, 44100, 48000, 96000), 0);
  const auto *enumerated = static_cast<const spa_pod *>(
      spa_pod_builder_pop(&enumBuilder, &enumFrame));

  if (!check(pipetune::appendPipeWireSampleRateConstraints(
                 discrete, constraints),
             "plain integer rate must be parsed") ||
      !check(pipetune::appendPipeWireSampleRateConstraints(
                 enumerated, constraints),
             "enumerated rates must be parsed")) {
    return false;
  }
  auto capabilities = pipetune::SampleRateCapabilities{
      .known = true, .constraints = std::move(constraints)};
  return check(pipetune::normalizeSampleRateCapabilities(capabilities),
               "parsed discrete rates must normalize") &&
         check(capabilities.constraints.size() == 3,
               "duplicate discrete rates must normalize away") &&
         check(capabilities.constraints[0].minimum == 44100 &&
                   capabilities.constraints[1].minimum == 48000 &&
                   capabilities.constraints[2].minimum == 96000,
               "enumerated rates differ");
}

static bool testRangeAndStepRates() {
  auto constraints = std::vector<pipetune::SampleRateConstraint>{};

  auto rangeStorage = std::array<std::uint8_t, 512>{};
  auto rangeBuilder = spa_pod_builder{};
  spa_pod_builder_init(&rangeBuilder, rangeStorage.data(),
                       rangeStorage.size());
  auto rangeFrame = spa_pod_frame{};
  spa_pod_builder_push_object(&rangeBuilder, &rangeFrame,
                              SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      &rangeBuilder, SPA_FORMAT_mediaType,
      SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_rate,
      SPA_POD_CHOICE_RANGE_Int(48000, 44100, 192000), 0);
  const auto *range = static_cast<const spa_pod *>(
      spa_pod_builder_pop(&rangeBuilder, &rangeFrame));

  auto stepStorage = std::array<std::uint8_t, 512>{};
  auto stepBuilder = spa_pod_builder{};
  spa_pod_builder_init(&stepBuilder, stepStorage.data(), stepStorage.size());
  auto stepFrame = spa_pod_frame{};
  spa_pod_builder_push_object(&stepBuilder, &stepFrame,
                              SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      &stepBuilder, SPA_FORMAT_mediaType,
      SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_rate,
      SPA_POD_CHOICE_STEP_Int(48000, 32000, 96000, 16000), 0);
  const auto *step = static_cast<const spa_pod *>(
      spa_pod_builder_pop(&stepBuilder, &stepFrame));

  return check(pipetune::appendPipeWireSampleRateConstraints(
                   range, constraints),
               "range rate must be parsed") &&
         check(pipetune::appendPipeWireSampleRateConstraints(
                   step, constraints),
               "step rate must be parsed") &&
         check(constraints.size() == 2,
               "range and step must each produce one constraint") &&
         check(constraints[0].kind ==
                       pipetune::SampleRateConstraintKind::range &&
                   constraints[0].minimum == 44100 &&
                   constraints[0].maximum == 192000 &&
                   constraints[0].step == 0,
               "parsed range differs") &&
         check(constraints[1].kind ==
                       pipetune::SampleRateConstraintKind::step &&
                   constraints[1].minimum == 32000 &&
                   constraints[1].maximum == 96000 &&
                   constraints[1].step == 16000,
               "parsed step differs");
}

static bool testIgnoredAndMalformedFormats() {
  auto constraints = std::vector<pipetune::SampleRateConstraint>{};

  auto videoStorage = std::array<std::uint8_t, 512>{};
  auto videoBuilder = spa_pod_builder{};
  spa_pod_builder_init(&videoBuilder, videoStorage.data(),
                       videoStorage.size());
  auto videoFrame = spa_pod_frame{};
  spa_pod_builder_push_object(&videoBuilder, &videoFrame,
                              SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(&videoBuilder, SPA_FORMAT_mediaType,
                      SPA_POD_Id(SPA_MEDIA_TYPE_video),
                      SPA_FORMAT_mediaSubtype,
                      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                      SPA_FORMAT_AUDIO_rate, SPA_POD_Int(48000), 0);
  const auto *video = static_cast<const spa_pod *>(
      spa_pod_builder_pop(&videoBuilder, &videoFrame));

  auto invalidStorage = std::array<std::uint8_t, 512>{};
  auto invalidBuilder = spa_pod_builder{};
  spa_pod_builder_init(&invalidBuilder, invalidStorage.data(),
                       invalidStorage.size());
  auto invalidFrame = spa_pod_frame{};
  spa_pod_builder_push_object(&invalidBuilder, &invalidFrame,
                              SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      &invalidBuilder, SPA_FORMAT_mediaType,
      SPA_POD_Id(SPA_MEDIA_TYPE_audio), SPA_FORMAT_mediaSubtype,
      SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_AUDIO_rate,
      SPA_POD_CHOICE_RANGE_Int(48000, 192000, 44100), 0);
  const auto *invalid = static_cast<const spa_pod *>(
      spa_pod_builder_pop(&invalidBuilder, &invalidFrame));

  return check(!pipetune::appendPipeWireSampleRateConstraints(
                   video, constraints),
               "non-audio formats must be ignored") &&
         check(!pipetune::appendPipeWireSampleRateConstraints(
                   invalid, constraints),
               "descending ranges must be rejected") &&
         check(constraints.empty(),
               "ignored formats must not append constraints");
}

static bool testReadableRateParameters() {
  const auto suspendedParameters = std::array{
      SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ),
      SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE)};
  const auto runningParameters = std::array{
      SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ),
      SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE)};
  const auto unrelatedParameters =
      std::array{SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_READWRITE)};

  return check(
             pipetune::pipeWireRateParameterAvailability(
                 suspendedParameters.data(), suspendedParameters.size()) ==
                 pipetune::PipeWireRateParameterAvailability{
                     .enumFormatReadable = true,
                     .formatReadable = false},
             "suspended nodes must not expose a readable active format") &&
         check(
             pipetune::pipeWireRateParameterAvailability(
                 runningParameters.data(), runningParameters.size()) ==
                 pipetune::PipeWireRateParameterAvailability{
                     .enumFormatReadable = true,
                     .formatReadable = true},
             "running nodes must expose both rate parameters") &&
         check(
             pipetune::pipeWireRateParameterAvailability(
                 unrelatedParameters.data(), unrelatedParameters.size()) ==
                 pipetune::PipeWireRateParameterAvailability{
                     .enumFormatReadable = false,
                     .formatReadable = false},
             "unrelated parameters must not be treated as rate information") &&
         check(
             pipetune::pipeWireRateParameterAvailability(nullptr, 1) ==
                 pipetune::PipeWireRateParameterAvailability{
                     .enumFormatReadable = false,
                     .formatReadable = false},
             "a missing parameter list must be treated as unreadable");
}

static bool testEnumerationEventsAreImmediatelyUsable() {
  auto storage = std::array<std::uint8_t, 512>{};
  auto builder = spa_pod_builder{};
  spa_pod_builder_init(&builder, storage.data(), storage.size());
  auto frame = spa_pod_frame{};
  spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format,
                              SPA_PARAM_EnumFormat);
  spa_pod_builder_add(
      &builder, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_audio),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_AUDIO_rate,
      SPA_POD_CHOICE_RANGE_Int(48000, 44100, 192000), 0);
  const auto *format =
      static_cast<const spa_pod *>(spa_pod_builder_pop(&builder, &frame));
  auto accumulated = std::vector<pipetune::SampleRateConstraint>{
      {.kind = pipetune::SampleRateConstraintKind::discrete,
       .minimum = 32000,
       .maximum = 32000,
       .step = 0}};

  const auto capabilities =
      pipetune::accumulatePipeWireSampleRateCapabilities(
          format, 0, accumulated);
  return check(capabilities.known,
               "one EnumFormat event must make capabilities known") &&
         check(capabilities.constraints.size() == 1,
               "a new enumeration must replace prior constraints") &&
         check(capabilities.constraints[0] ==
                   pipetune::SampleRateConstraint{
                       .kind = pipetune::SampleRateConstraintKind::range,
                       .minimum = 44100,
                       .maximum = 192000,
                       .step = 0},
               "the current EnumFormat event must be immediately usable");
}

int main() {
  return testDiscreteAndEnumRates() && testRangeAndStepRates() &&
                 testIgnoredAndMalformedFormats() &&
                 testReadableRateParameters() &&
                 testEnumerationEventsAreImmediatelyUsable()
             ? 0
             : 1;
}
