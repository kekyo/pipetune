/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "effetune_backend_abi.h"

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
  auto *staging = et_instance_asset_begin_ptr(
      engine, instance, slot, info->channels, info->frames, info->topology,
      info->head_block, info->rate_divider, info->path_count, info->input_count,
      info->processing_channels, info->footprint_bytes, info->byte_size, 0.0);
  if (staging == nullptr) {
    return ET_ERR_STATE;
  }
  // The staging address remains entirely inside this shared library. Only the
  // caller-owned bytes and status cross PipeTune's backend boundary.
  std::memcpy(staging, payload, static_cast<std::size_t>(payloadBytes));
  const auto status = et_instance_asset_commit(engine, instance, slot, info->byte_size, formatTag);
  if (status != ET_OK) et_instance_asset_abort(engine, instance, slot);
  return status;
}
