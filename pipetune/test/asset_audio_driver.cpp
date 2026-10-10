/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_audio_decoder.h"

#include <bit>
#include <iostream>
#include <iterator>

int main(int argc, char **argv) {
  if (argc != 3) return 2;
  try {
    const auto budget = static_cast<std::size_t>(std::stoull(argv[1]));
    const auto rate = static_cast<std::uint32_t>(std::stoul(argv[2]));
    const auto bytes = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(std::cin), {});
    auto audio = pipetune::decodeAssetAudio(bytes, budget);
    if (rate != 0) audio = pipetune::resampleIrAudio(std::move(audio), rate, budget);
    std::cout << audio.sampleRate << ' ' << audio.channels.size() << ' ' << audio.channels.front().size() << '\n';
    for (const auto &channel : audio.channels)
      for (const auto sample : channel) {
        const auto word = std::bit_cast<std::uint32_t>(sample);
        for (auto byte = 0u; byte < 4u; ++byte) std::cout.put(static_cast<char>(word >> (byte * 8u)));
      }
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
