/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GENERATED_FIR_ASSET_H
#define PIPETUNE_GENERATED_FIR_ASSET_H

#include "effetune_backend_abi.h"

#include <yyjson.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune {

struct DspBackendApi;

struct GeneratedFirAsset {
  std::vector<std::uint8_t> payload;
  pipetune_effetune_asset_info_v1 info{};
  std::uint32_t formatTag = ET_ASSET_F32_MULTICH;
  std::uint32_t bandCount = 0u;
  std::uint32_t filterDelaySamples = 0u;
  std::string omissionReason;
  std::string error;
};

bool supportsGeneratedFirAsset(std::string_view displayName) noexcept;

GeneratedFirAsset designGeneratedFirAsset(
    std::string_view displayName, yyjson_val *parameters, float sampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api);

} // namespace pipetune

#endif
