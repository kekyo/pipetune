/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "effetune_backend_abi.h"

#include "effetune_backend_engine_access.h"
#include "engine.h"

#include <cstdint>
#include <cstring>
#include <limits>

extern "C" et_status pipetune_effetune_instance_asset_copy_v1(
    et_engine engine, et_instance instance, std::uint32_t slot,
    const pipetune_effetune_asset_info_v1 *info,
    const std::uint8_t *payload, std::uint64_t payloadBytes,
    std::uint32_t formatTag) {
  if (info == nullptr || payload == nullptr || payloadBytes == 0u ||
      payloadBytes > std::numeric_limits<std::uint32_t>::max() ||
      payloadBytes != info->byte_size) {
    return ET_ERR_ARGS;
  }
  auto *target = pipetuneEffetuneBackendResolveEngine(engine);
  if (target == nullptr) {
    return ET_ERR_ARGS;
  }
  const auto beginInfo = effetune::AssetBeginInfo{
      .channels = info->channels,
      .frames = info->frames,
      .topology = info->topology,
      .headBlock = info->head_block,
      .rateDivider = info->rate_divider,
      .pathCount = info->path_count,
      .inputCount = info->input_count,
      .processingChannels = info->processing_channels,
      .footprintBytes = info->footprint_bytes,
      .byteSize = info->byte_size,
  };
  auto *staging = target->beginInstanceAsset(instance, slot, beginInfo);
  if (staging == nullptr) {
    return ET_ERR_STATE;
  }
  // The staging address remains entirely inside this shared library. Only the
  // caller-owned bytes and status cross PipeTune's backend boundary.
  std::memcpy(staging, payload, static_cast<std::size_t>(payloadBytes));
  return target->commitInstanceAsset(instance, slot, info->byte_size,
                                     formatTag);
}
