/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>

namespace pipetune {

static yyjson_val *getObjectMember(yyjson_val *object, std::string_view key) {
  if (!yyjson_is_obj(object) || key.empty()) {
    return nullptr;
  }
  return yyjson_obj_getn(object, key.data(), key.size());
}

static yyjson_val *readElementSource(yyjson_val *parameters, const ParameterElement &element) {
  auto *direct = getObjectMember(parameters, element.directKey);
  if (!element.arrayKey.empty()) {
    auto *array = getObjectMember(parameters, element.arrayKey);
    return yyjson_is_arr(array) ? yyjson_arr_get(array, element.containerIndex) : direct;
  }
  if (!element.objectArrayKey.empty()) {
    auto *array = getObjectMember(parameters, element.objectArrayKey);
    if (!yyjson_is_arr(array)) {
      return direct;
    }
    auto *entry = yyjson_arr_get(array, element.containerIndex);
    return getObjectMember(entry, element.memberKey);
  }
  return direct;
}

static bool readSafeInteger(yyjson_val *value, double &output) {
  constexpr auto maximumSafeInteger = std::uint64_t{9007199254740991ULL};
  constexpr auto minimumSafeInteger = std::int64_t{-9007199254740991LL};
  if (yyjson_is_uint(value)) {
    const auto parsed = yyjson_get_uint(value);
    if (parsed > maximumSafeInteger) {
      return false;
    }
    output = static_cast<double>(parsed);
    return true;
  }
  if (yyjson_is_sint(value)) {
    const auto parsed = yyjson_get_sint(value);
    if (parsed < minimumSafeInteger ||
        parsed > static_cast<std::int64_t>(maximumSafeInteger)) {
      return false;
    }
    output = static_cast<double>(parsed);
    return true;
  }
  if (yyjson_is_real(value)) {
    const auto parsed = yyjson_get_real(value);
    if (!std::isfinite(parsed) || std::trunc(parsed) != parsed ||
        std::abs(parsed) > static_cast<double>(maximumSafeInteger)) {
      return false;
    }
    output = parsed;
    return true;
  }
  return false;
}

static float packElement(yyjson_val *value, const ParameterElement &element,
                         std::string &error) {
  if (element.kind == ParameterKind::boolean) {
    if (yyjson_is_bool(value)) {
      return yyjson_get_bool(value) ? 1.0F : 0.0F;
    }
    if (yyjson_is_num(value)) {
      const auto number = yyjson_get_num(value);
      if (number == 1.0) {
        return 1.0F;
      }
      if (number == 0.0) {
        return 0.0F;
      }
    }
    return static_cast<float>(element.defaultValue);
  }

  if (element.kind == ParameterKind::enumeration) {
    if (value == nullptr) {
      return static_cast<float>(element.defaultValue);
    }
    if (yyjson_is_str(value)) {
      const auto text = std::string_view(yyjson_get_str(value), yyjson_get_len(value));
      const auto match = std::ranges::find(element.enumerationValues, text);
      if (match != element.enumerationValues.end()) {
        return static_cast<float>(std::distance(element.enumerationValues.begin(), match));
      }
    }
    if (element.rejectInvalid) {
      error = "invalid enum parameter " + std::string(element.directKey);
    }
    return static_cast<float>(element.defaultValue);
  }

  auto number = 0.0;
  const auto valid = element.kind == ParameterKind::integer
                         ? readSafeInteger(value, number)
                         : yyjson_is_num(value) && std::isfinite(number = yyjson_get_num(value));
  if (!valid) {
    number = element.defaultValue;
  } else {
    number = std::clamp(number, element.minimum, element.maximum);
  }
  return static_cast<float>(number);
}

static std::vector<std::uint8_t> packMatrixRoutes(const StructuredParameter &structured,
                                                  yyjson_val *parameters, std::string &error) {
  auto source = structured.defaultValue;
  auto *value = getObjectMember(parameters, structured.key);
  if (yyjson_is_str(value)) {
    source = std::string_view(yyjson_get_str(value), yyjson_get_len(value));
  }

  auto routes = std::vector<std::uint8_t>();
  routes.reserve(std::min<std::size_t>(source.size(), structured.maxItems * 3u));
  auto offset = std::size_t{0};
  while (offset < source.size()) {
    auto phase = std::uint8_t{0};
    if (source[offset] == 'p') {
      phase = 1;
      ++offset;
    }
    if (offset + 1 >= source.size()) {
      break;
    }
    const auto input = source[offset];
    const auto output = source[offset + 1];
    if (input >= '0' && input <= '8' && output >= '0' && output <= '8') {
      if (routes.size() / 3u >= structured.maxItems) {
        error = "structured parameter capacity exceeded";
        return {};
      }
      routes.push_back(static_cast<std::uint8_t>(input - '0'));
      routes.push_back(static_cast<std::uint8_t>(output - '0'));
      routes.push_back(phase);
    }
    offset += 2;
  }

  const auto routeCount = routes.size() / 3u;
  auto packed = std::vector<std::uint8_t>();
  packed.reserve(4u + routes.size());
  packed.push_back(1);
  packed.push_back(0);
  packed.push_back(static_cast<std::uint8_t>(routeCount & 0xffu));
  packed.push_back(static_cast<std::uint8_t>((routeCount >> 8u) & 0xffu));
  packed.insert(packed.end(), routes.begin(), routes.end());
  return packed;
}

const DspDefinition *findDspByDisplayName(std::string_view displayName) {
  const auto catalog = generatedDspCatalog();
  const auto match = std::ranges::find(catalog, displayName, &DspDefinition::displayName);
  return match == catalog.end() ? nullptr : &*match;
}

const DspDefinition *findDspByTypeName(std::string_view typeName) {
  const auto catalog = generatedDspCatalog();
  const auto match = std::ranges::find(catalog, typeName, &DspDefinition::typeName);
  return match == catalog.end() ? nullptr : &*match;
}

PackedParameters packDspParameters(const DspDefinition &definition, yyjson_val *parameters) {
  auto packed =
      PackedParameters{.definition = &definition, .floats = {}, .bytes = {}, .error = {}};
  packed.floats.reserve(definition.elements.size());
  for (const auto &element : definition.elements) {
    packed.floats.push_back(
        packElement(readElementSource(parameters, element), element, packed.error));
  }
  if (definition.structured.present) {
    packed.bytes = packMatrixRoutes(definition.structured, parameters, packed.error);
  }
  return packed;
}

} // namespace pipetune
