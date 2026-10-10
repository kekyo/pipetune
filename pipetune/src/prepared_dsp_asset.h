/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PREPARED_DSP_ASSET_H
#define PIPETUNE_PREPARED_DSP_ASSET_H
#include "effetune_backend_abi.h"
#include <cstdint>
#include <string>
#include <vector>

namespace pipetune {

/** Owned host-side data transferred through the native asset copy ABI. */
struct PreparedDspAsset {
  std::vector<std::uint8_t> payload; /**< Encoded asset bytes; released off the audio thread. */
  pipetune_effetune_asset_info_v1 info{}; /**< Native dimensions, topology, and allocation footprint. */
  std::uint32_t formatTag = ET_ASSET_F32_MULTICH; /**< Native asset format identifier. */
  std::uint32_t bandCount = 0; /**< Generated crossover band count, or zero. */
  std::uint32_t filterDelaySamples = 0; /**< Generated filter group delay in processing frames. */
  std::string omissionReason; /**< Non-fatal reason to omit the node, if applicable. */
  std::string error; /**< Fatal preparation diagnostic, or empty on success. */
};

} // namespace pipetune
#endif
