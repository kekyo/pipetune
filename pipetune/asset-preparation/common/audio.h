/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_AUDIO_H
#define PIPETUNE_ASSET_AUDIO_H

#include <cstdint>
#include <vector>

namespace pipetune::assets {

/** Owns finite channel-major float32 audio at its original sample rate. */
struct Audio {
  /** Sample rate in hertz. */
  std::uint32_t sampleRate = 0;
  /** Equal-length channels in their original order. */
  std::vector<std::vector<float>> channels;
};

} // namespace pipetune::assets
#endif
