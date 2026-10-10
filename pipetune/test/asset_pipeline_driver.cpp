/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipetune/dsp_pipeline.h>

#include <algorithm>
#include <iomanip>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 6 && argc != 7) return 2;
  const auto rate = std::stof(argv[2]);
  const auto channels = static_cast<std::uint32_t>(std::stoul(argv[3]));
  const auto frames = static_cast<std::uint32_t>(std::stoul(argv[4]));
  const auto inputChannel = static_cast<std::uint32_t>(std::stoul(argv[5]));
  const auto kind = argc == 7 && std::string_view(argv[6]) == "simd" ?
      pipetune::DspBackendKind::simd : pipetune::DspBackendKind::scalar;
  const auto backend = pipetune::loadDspBackend(kind);
  if (backend.backend == nullptr) { std::cerr << backend.error << '\n'; return 1; }
  const auto loaded = pipetune::loadDspPipeline(argv[1],
      {.sampleRate = rate, .maxChannels = channels, .maxFrames = 128u}, backend.backend);
  if (loaded.pipeline == nullptr) {
    std::cerr << loaded.error << '\n';
    return 1;
  }
  for (const auto &warning : loaded.warnings) std::cerr << warning.reason << '\n';
  std::cerr << "cacheHits=" << loaded.cachedAssetCount << '\n';
  std::cout << loaded.pipeline->activePluginCount() << ' '
            << loaded.pipeline->latencyFrames() << '\n' << std::setprecision(9);
  auto output = std::vector<float>(channels * frames);
  auto block = std::vector<float>(channels * 128u);
  for (auto offset = 0u; offset < frames; offset += 128u) {
    const auto count = std::min(128u, frames - offset);
    std::fill(block.begin(), block.end(), 0.0F);
    if (offset == 0u && inputChannel < channels) block[inputChannel * count] = 1.0F;
    if (loaded.pipeline->process(std::span(block).first(channels * count), channels,
          count, offset / static_cast<double>(rate)) != pipetune::ProcessStatus::ok) return 3;
    for (auto channel = 0u; channel < channels; ++channel)
      std::copy_n(block.begin() + channel * count, count,
                  output.begin() + channel * frames + offset);
  }
  for (const auto sample : output) std::cout << sample << '\n';
  return 0;
}
