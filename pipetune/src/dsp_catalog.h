/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_DSP_CATALOG_H
#define PIPETUNE_DSP_CATALOG_H

#include <yyjson.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune {

enum class ParameterKind {
  floating,
  integer,
  boolean,
  enumeration
};

struct ParameterElement {
  std::string_view directKey;
  std::string_view arrayKey;
  std::string_view objectArrayKey;
  std::string_view memberKey;
  std::size_t containerIndex;
  ParameterKind kind;
  double minimum;
  double maximum;
  double defaultValue;
  std::span<const std::string_view> enumerationValues;
  bool rejectInvalid;
};

struct StructuredParameter {
  bool present;
  std::string_view key;
  std::string_view defaultValue;
  std::uint32_t maxItems;
};

struct DspDefinition {
  std::string_view displayName;
  std::string_view typeName;
  std::uint32_t hash;
  std::uint32_t floatCount;
  std::span<const ParameterElement> elements;
  StructuredParameter structured;
  std::uint32_t paramBytesCapacity;
  std::array<std::uint32_t, 32> assetCapacities;
  bool requiresExternalAssets;
};

struct PackedParameters {
  const DspDefinition *definition;
  std::vector<float> floats;
  std::vector<std::uint8_t> bytes;
  std::string error;
};

std::span<const DspDefinition> generatedDspCatalog();
const DspDefinition *findDspByDisplayName(std::string_view displayName);
const DspDefinition *findDspByTypeName(std::string_view typeName);
PackedParameters packDspParameters(const DspDefinition &definition, yyjson_val *parameters);

} // namespace pipetune

#endif
