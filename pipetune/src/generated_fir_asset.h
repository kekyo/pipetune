/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_GENERATED_FIR_ASSET_H
#define PIPETUNE_GENERATED_FIR_ASSET_H

#include "prepared_dsp_asset.h"

#include <yyjson.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune {

struct DspBackendApi;
struct CrosstalkMeasurements;
struct BassManagementConfig;


PreparedDspAsset designCrosstalkAsset(const CrosstalkMeasurements &measurements,
                                      yyjson_val *parameters, float sampleRate,
                                      std::uint32_t processingChannels);

PreparedDspAsset designBassManagementAsset(
    const BassManagementConfig &config, float sampleRate,
    std::uint32_t maxFrames, const DspBackendApi &api);

bool supportsGeneratedFirAsset(std::string_view displayName) noexcept;

PreparedDspAsset designGeneratedFirAsset(
    std::string_view displayName, yyjson_val *parameters, float sampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api);

} // namespace pipetune

#endif
