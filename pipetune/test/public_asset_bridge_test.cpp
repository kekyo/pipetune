/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "effetune_backend_abi.h"
#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>

// Link the production bridge with only public ABI functions. A private Engine
// dependency must not be necessary for a complete or rejected transfer.
static auto staging = std::array<std::uint8_t, 16>{};
static auto active = std::array<std::uint8_t, 16>{};
static bool aborted = false;
static bool rejectCommit = false;
extern "C" std::uint8_t *et_instance_asset_begin_ptr(et_engine engine, et_instance instance,
    std::uint32_t slot, std::uint32_t channels, std::uint32_t frames,
    std::uint32_t topology, std::uint32_t headBlock, std::uint32_t rateDivider,
    std::uint32_t pathCount, std::uint32_t inputCount, std::uint32_t processingChannels,
    std::uint32_t footprintBytes, std::uint32_t byteSize, double minimumPhysicalBpm) {
  return engine == 1 && instance == 2 && slot == 0 && channels == 1 && frames == 4 &&
      topology == 0 && headBlock == 0 && rateDivider == 1 && pathCount == 0 && inputCount == 0 &&
      processingChannels == 2 && footprintBytes == 32 && byteSize == 16 && minimumPhysicalBpm == 0 ? staging.data() : nullptr;
}
extern "C" et_status et_instance_asset_commit(et_engine engine, et_instance instance,
    std::uint32_t slot, std::uint32_t byteSize, std::uint32_t formatTag) {
  if (rejectCommit || engine != 1 || instance != 2 || slot != 0 || byteSize != 16 || formatTag != 1)
    return ET_ERR_ARGS;
  active = staging;
  return ET_OK;
}
extern "C" void et_instance_asset_abort(et_engine, et_instance, std::uint32_t) { aborted = true; }
int main() {
  const auto info = pipetune_effetune_asset_info_v1{1,4,0,0,1,0,0,2,32,16};
  auto source = std::array<std::uint8_t,16>{1,2,3,4,5};
  if (pipetune_effetune_instance_asset_copy_v1(1,2,0,&info,source.data(),source.size(),1) != ET_OK || active != source)
    return 1;
  source.fill(7);
  rejectCommit = true;
  if (pipetune_effetune_instance_asset_copy_v1(1,2,0,&info,source.data(),source.size(),1) != ET_ERR_ARGS ||
      !aborted) return 2;
  if (pipetune_effetune_instance_asset_copy_v1(1,2,0,&info,source.data(),15,1) != ET_ERR_ARGS ||
      pipetune_effetune_instance_asset_copy_v1(0,2,0,&info,source.data(),source.size(),1) != ET_ERR_STATE)
    return 3;
  return 0;
}
