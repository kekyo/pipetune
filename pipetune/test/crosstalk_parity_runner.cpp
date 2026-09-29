/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipetune/dsp_pipeline.h>
#include <pipetune/dsp_backend.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <vector>

int main(int argc, char **argv) {
  if (argc != 9 && argc != 10) return 2;
  const auto rate = std::stof(argv[4]);
  const auto taps = static_cast<unsigned>(std::stoul(argv[5]));
  const auto head = static_cast<unsigned>(std::stoul(argv[6]));
  const auto strength = std::stof(argv[7]) / 100.0F;
  const auto gain = std::pow(10.0F, std::stof(argv[8]) / 20.0F);
  auto expected = std::vector<float>(4u * taps);
  std::ifstream input(argv[3], std::ios::binary);
  input.read(reinterpret_cast<char *>(expected.data()), expected.size() * sizeof(float));
  if (!input) return 2;
  const auto options = pipetune::PipelineBuildOptions{rate, 2u, 128u};
  for (const auto kind : {pipetune::DspBackendKind::scalar, pipetune::DspBackendKind::simd}) {
    auto backend = pipetune::loadDspBackend(kind);
    if (!backend.backend) { std::cerr << backend.error; return 1; }
    auto loaded = pipetune::loadDspPipeline(argv[1], options, backend.backend, {argv[2]});
    if (argc == 10) {
      const auto expectWarning = std::string_view(argv[9]) == "skip";
      if (!loaded.pipeline || loaded.pipeline->activePluginCount() != 0u || loaded.warnings.empty() == expectWarning) {
        std::cerr << "Invalid or disabled Crosstalk must be omitted with the expected diagnostics\n"; return 1;
      }
      continue;
    }
    if (!loaded.pipeline || !loaded.warnings.empty() || loaded.pipeline->activePluginCount() != 1u) {
      std::cerr << "Crosstalk pipeline must activate: " << loaded.error << '\n';
      for (const auto &warning : loaded.warnings) std::cerr << warning.reason << '\n';
      return 1;
    }
    if (loaded.pipeline->latencyFrames() != head + taps / 2u || loaded.measurementFiles.size() != 2u) {
      std::cerr << "Crosstalk latency/dependencies differ\n"; return 1;
    }
    // Warm the upstream incremental convolver before injecting the test signal.
    auto block = std::vector<float>(256u, 0.0F);
    for (unsigned frame = 0; frame < 262144u; frame += 128u) {
      std::fill(block.begin(), block.end(), 0.0F);
      if (loaded.pipeline->process(block, 2u, 128u, frame / rate) != pipetune::ProcessStatus::ok) return 1;
    }
    for (unsigned frame = 0; frame < taps + head + 256u; frame += 128u) {
      std::fill(block.begin(), block.end(), 0.0F);
      if (frame == 0) { block[0] = 1.0F; block[128u + 32u] = 0.5F; }
      if (loaded.pipeline->process(block, 2u, 128u, (262144u + frame) / rate) != pipetune::ProcessStatus::ok) return 1;
      for (unsigned channel = 0; channel < 2; ++channel) {
        for (unsigned i = 0; i < 128; ++i) {
          const auto sample = static_cast<int>(frame + i) - static_cast<int>(head);
          float wet = 0;
          if (sample >= 0 && sample < static_cast<int>(taps)) wet += expected[channel * taps + sample];
          if (sample >= 32 && sample - 32 < static_cast<int>(taps)) wet += 0.5F * expected[(2u + channel) * taps + sample - 32];
          const auto dry = sample == static_cast<int>(taps / 2u + channel * 32u) ? (channel == 0 ? 1.0F : 0.5F) : 0.0F;
          const auto reference = (dry + strength * (wet - dry)) * gain;
          const auto actual = block[channel * 128u + i];
          if (!std::isfinite(actual) || std::abs(actual - reference) > 2.0e-4F) {
            std::cerr << "Crosstalk output differs at " << frame + i << "/" << channel << ": " << actual << " vs " << reference << '\n'; return 1;
          }
        }
      }
    }
    // Rebuilding must retain the measurement directory, even without GTK.
    auto rebuilt = pipetune::rebuildDspPipeline(*loaded.pipeline, options);
    if (!rebuilt.pipeline || rebuilt.pipeline->activePluginCount() != 1u || !rebuilt.warnings.empty()) return 1;
  }
  return 0;
}
