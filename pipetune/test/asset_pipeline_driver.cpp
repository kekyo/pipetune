/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipetune/dsp_pipeline.h>

#include <algorithm>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
  if (argc < 6 || argc > 9 || (argc == 9 && std::string_view(argv[8]) != "rebuild")) return 2;
  const auto rate = std::stof(argv[2]);
  const auto channels = static_cast<std::uint32_t>(std::stoul(argv[3]));
  const auto frames = static_cast<std::uint32_t>(std::stoul(argv[4]));
  const auto inputChannel = static_cast<std::uint32_t>(std::stoul(argv[5]));
  const auto kind = argc >= 7 && std::string_view(argv[6]) == "simd" ?
      pipetune::DspBackendKind::simd : pipetune::DspBackendKind::scalar;
  const auto backend = pipetune::loadDspBackend(kind);
  if (backend.backend == nullptr) { std::cerr << backend.error << '\n'; return 1; }
  auto context = pipetune::defaultPipelineLoadContext();
  if (const auto *value = std::getenv("PIPETUNE_TEST_SFZ_BYTES")) context.sfzMaximumBytes = std::stoull(value);
  if (const auto *value = std::getenv("PIPETUNE_TEST_ASSET_MEMORY_BYTES")) context.assetMemoryBytes = std::stoull(value);
  const auto started = std::chrono::steady_clock::now();
  const auto options = pipetune::PipelineBuildOptions{rate, channels, 128u};
  const auto initial = argc == 9 ? pipetune::PipelineBuildOptions{
      rate == 48000 ? 96000.0F : 48000.0F, channels == 16 ? 4u : 16u, 128u} : options;
  auto loaded = pipetune::loadDspPipeline(argv[1], initial, backend.backend, context);
  if (loaded.pipeline && argc == 9) {
    // Keep the complete older engine alive until the replacement is ready.
    // This observes combined memory admission as well as retained source roots.
    auto rebuilt = pipetune::rebuildDspPipeline(*loaded.pipeline, options, backend.backend);
    if (rebuilt.pipeline && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return 3;
    loaded = std::move(rebuilt);
  }
  if (loaded.pipeline == nullptr) {
    std::cerr << loaded.error << '\n';
    return 1;
  }
  for (const auto &warning : loaded.warnings) std::cerr << warning.reason << '\n';
  std::cerr << "cacheHits=" << loaded.cachedAssetCount << '\n';
  std::cerr << "dependencies=" << loaded.pipeline->dependencyFiles().size() << '\n';
  std::cerr << "preparationMs=" << std::chrono::duration<double, std::milli>(
      std::chrono::steady_clock::now() - started).count() << '\n';
  std::cout << loaded.pipeline->activePluginCount() << ' '
            << loaded.pipeline->latencyFrames() << '\n' << std::setprecision(9);
  auto output = std::vector<float>(channels * frames);
  auto block = std::vector<float>(channels * 128u);
  const auto frequency = argc >= 8 ? std::stod(argv[7]) : 0.0;
  for (auto pass = 0; pass < (argc == 9 ? 2 : 1); ++pass) {
    if (pass && loaded.pipeline->reset() != pipetune::ProcessStatus::ok) return 3;
    for (auto offset = 0u; offset < frames; offset += 128u) {
      const auto count = std::min(128u, frames - offset);
      std::fill(block.begin(), block.end(), 0.0F);
      if (offset == 0u && inputChannel < channels) block[inputChannel * count] = 1.0F;
      if (frequency > 0 && inputChannel < channels)
        for (auto frame = 0u; frame < count; ++frame)
          block[inputChannel * count + frame] = static_cast<float>(0.3 * std::sin(2 * 3.141592653589793 * frequency * (offset + frame) / rate));
      if (loaded.pipeline->process(std::span(block).first(channels * count), channels,
            count, offset / static_cast<double>(rate)) != pipetune::ProcessStatus::ok) return 3;
      for (auto channel = 0u; channel < channels; ++channel) {
        if (pass) {
          for (auto frame = 0u; frame < count; ++frame)
            if (std::abs(output[channel * frames + offset + frame] - block[channel * count + frame]) > 1e-6F) {
              std::cerr << "reset changed prepared PCM at " << offset + frame << '/' << channel << '\n'; return 3;
            }
        } else
        std::copy_n(block.begin() + channel * count, count,
                    output.begin() + channel * frames + offset);
      }
    }
  }
  for (const auto sample : output) std::cout << sample << '\n';
  return 0;
}
