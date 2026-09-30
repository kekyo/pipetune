/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_BASS_MANAGEMENT_CONFIG_H
#define PIPETUNE_BASS_MANAGEMENT_CONFIG_H

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace pipetune {

struct PackedParameters;

// Effective filter configuration, decoded only from the values sent to the DSP.
// The kernel normalizes all roles/routes away when no sub output is selected.
struct BassManagementConfig {
  bool linear = false;
  std::uint32_t taps = 0;
  std::uint32_t processingChannels = 0;
  std::array<float, 16> frequencies{};
  std::array<std::uint32_t, 16> slopes{};
  std::vector<std::uint32_t> lowpassInputs;
  std::string error;
};

BassManagementConfig decodeBassManagementConfig(const PackedParameters &packed,
                                                std::uint32_t processingChannels);

} // namespace pipetune

#endif
