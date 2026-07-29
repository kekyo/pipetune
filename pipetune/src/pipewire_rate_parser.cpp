#include "pipewire_rate_parser.h"

#include <spa/param/format-utils.h>
#include <spa/param/format.h>
#include <spa/pod/iter.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace pipetune {

PipeWireRateParameterAvailability pipeWireRateParameterAvailability(
    const spa_param_info *parameters, std::uint32_t parameterCount) {
  auto availability = PipeWireRateParameterAvailability{
      .enumFormatReadable = false, .formatReadable = false};
  if (parameters == nullptr) {
    return availability;
  }
  for (auto index = std::uint32_t{0}; index < parameterCount; ++index) {
    if ((parameters[index].flags & SPA_PARAM_INFO_READ) == 0) {
      continue;
    }
    if (parameters[index].id == SPA_PARAM_EnumFormat) {
      availability.enumFormatReadable = true;
    } else if (parameters[index].id == SPA_PARAM_Format) {
      availability.formatReadable = true;
    }
  }
  return availability;
}

static bool readChoiceValues(const spa_pod &pod, std::uint32_t required,
                             std::vector<std::int32_t> &values) {
  if (!spa_pod_is_choice(&pod)) {
    return false;
  }
  const auto &choice =
      *reinterpret_cast<const spa_pod_choice *>(&pod);
  const auto childSize = choice.body.child.size;
  if (choice.body.child.type != SPA_TYPE_Int ||
      childSize != sizeof(std::int32_t) ||
      pod.size < sizeof(spa_pod_choice_body)) {
    return false;
  }
  const auto count =
      (pod.size - sizeof(spa_pod_choice_body)) / childSize;
  if (count < required) {
    return false;
  }
  const auto *contents =
      reinterpret_cast<const std::uint8_t *>(&choice.body) +
      sizeof(spa_pod_choice_body);
  values.resize(count);
  for (auto index = std::uint32_t{0}; index < count; ++index) {
    std::memcpy(&values[index], contents + index * childSize,
                sizeof(values[index]));
  }
  return true;
}

static bool appendDiscrete(std::int32_t value,
                           std::vector<SampleRateConstraint> &output) {
  if (value <= 0) {
    return false;
  }
  const auto rate = static_cast<std::uint32_t>(value);
  output.push_back({.kind = SampleRateConstraintKind::discrete,
                    .minimum = rate,
                    .maximum = rate,
                    .step = 0});
  return true;
}

bool appendPipeWireSampleRateConstraints(
    const spa_pod *format,
    std::vector<SampleRateConstraint> &constraints) {
  if (format == nullptr) {
    return false;
  }
  auto mediaType = std::uint32_t{0};
  auto mediaSubtype = std::uint32_t{0};
  if (spa_format_parse(format, &mediaType, &mediaSubtype) < 0 ||
      mediaType != SPA_MEDIA_TYPE_audio ||
      mediaSubtype != SPA_MEDIA_SUBTYPE_raw) {
    return false;
  }
  const auto *property =
      spa_pod_find_prop(format, nullptr, SPA_FORMAT_AUDIO_rate);
  if (property == nullptr) {
    return false;
  }

  auto parsed = std::vector<SampleRateConstraint>{};
  if (!spa_pod_is_choice(&property->value)) {
    auto rate = std::int32_t{0};
    if (spa_pod_get_int(&property->value, &rate) < 0 ||
        !appendDiscrete(rate, parsed)) {
      return false;
    }
  } else {
    const auto &choice = *reinterpret_cast<const spa_pod_choice *>(
        &property->value);
    auto values = std::vector<std::int32_t>{};
    switch (choice.body.type) {
    case SPA_CHOICE_None:
      if (!readChoiceValues(property->value, 1, values) ||
          !appendDiscrete(values[0], parsed)) {
        return false;
      }
      break;
    case SPA_CHOICE_Enum:
      if (!readChoiceValues(property->value, 1, values)) {
        return false;
      }
      for (const auto value : values) {
        if (!appendDiscrete(value, parsed)) {
          return false;
        }
      }
      break;
    case SPA_CHOICE_Range:
      if (!readChoiceValues(property->value, 3, values) ||
          values[1] <= 0 || values[2] < values[1]) {
        return false;
      }
      parsed.push_back(
          {.kind = SampleRateConstraintKind::range,
           .minimum = static_cast<std::uint32_t>(values[1]),
           .maximum = static_cast<std::uint32_t>(values[2]),
           .step = 0});
      break;
    case SPA_CHOICE_Step:
      if (!readChoiceValues(property->value, 4, values) ||
          values[1] <= 0 || values[2] < values[1] || values[3] <= 0) {
        return false;
      }
      parsed.push_back(
          {.kind = SampleRateConstraintKind::step,
           .minimum = static_cast<std::uint32_t>(values[1]),
           .maximum = static_cast<std::uint32_t>(values[2]),
           .step = static_cast<std::uint32_t>(values[3])});
      break;
    default:
      return false;
    }
  }
  constraints.insert(constraints.end(), parsed.begin(), parsed.end());
  return !parsed.empty();
}

SampleRateCapabilities accumulatePipeWireSampleRateCapabilities(
    const spa_pod *format, std::uint32_t index,
    std::vector<SampleRateConstraint> &constraints) {
  if (index == 0) {
    constraints.clear();
  }
  static_cast<void>(
      appendPipeWireSampleRateConstraints(format, constraints));
  auto capabilities =
      SampleRateCapabilities{.known = true, .constraints = constraints};
  if (!normalizeSampleRateCapabilities(capabilities)) {
    capabilities = {.known = true, .constraints = {}};
  }
  return capabilities;
}

} // namespace pipetune
