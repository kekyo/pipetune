/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_catalog.h"
#include <pipetune/dsp_backend.h>
#include <pipetune/dsp_pipeline.h>
#include <pipetune/version.h>

#include <array>
#include <cmath>
#include <iostream>
#include <vector>

static int failures = 0;

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
    ++failures;
  }
  return condition;
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  check(pipetune::effetuneVersion() == "2.13.0", "the embedded app version must be 2.13.0");
  check(pipetune::generatedDspCatalog().size() == 112, "the app catalog must contain 112 DSPs");
  struct Contract {
    const char *type;
    std::uint32_t hash;
    std::uint32_t floats;
    std::uint32_t assetBytes;
  };
  for (const auto &contract : std::array{
           Contract{"AdaptivePredictionEffectPlugin", 0xebd8a6f0u, 10u, 0u},
           Contract{"SFZNotePlayerPlugin", 0x0f17627cu, 16u, 1073741824u},
           Contract{"CassetteArtifactsPlugin", 0x328491aeu, 13u, 0u},
           Contract{"NoteSpectrogramPlugin", 0x9d70750bu, 2u, 0u}}) {
    const auto *definition = pipetune::findDspByTypeName(contract.type);
    if (!check(definition != nullptr, contract.type)) continue;
    check(definition->hash == contract.hash && definition->floatCount == contract.floats &&
              definition->assetCapacities[0] == contract.assetBytes &&
              definition->requiresExternalAssets == (contract.assetBytes != 0),
          "new app parameter and asset contracts must match the native kernels");
  }

  const auto discovered = pipetune::discoverDspBackends();
  auto backends = std::vector{discovered.scalar.backend};
  for (const auto &variant : discovered.simdVariants)
    if (variant.backend != nullptr) backends.push_back(variant.backend);
  for (const auto &backend : backends) {
    if (!check(backend != nullptr, "the scalar backend must be available")) continue;
    const auto loaded = pipetune::loadDspPipeline(argv[1],
        {.sampleRate = 48000.0F, .maxChannels = 2u, .maxFrames = 64u}, backend);
    if (!check(loaded.pipeline != nullptr, loaded.error)) continue;
    check(loaded.warnings.empty() && loaded.pipeline->activePluginCount() == 1u &&
              loaded.pipeline->latencyFrames() == 0u,
          "Adaptive Prediction must execute without an asset or added latency");
    auto audio = std::array<float, 128>{};
    for (auto block = 0u; block < 20u; ++block) {
      for (auto frame = 0u; frame < 64u; ++frame) {
        audio[frame] = 0.125F;
        audio[64u + frame] = -0.25F;
      }
      check(loaded.pipeline->process(audio, 2u, 64u, block * 64.0 / 48000.0) ==
                pipetune::ProcessStatus::ok,
            "the minimal 2.13 preset must process PCM");
    }
    for (auto frame = 0u; frame < 64u; ++frame)
      check(std::abs(audio[frame] - 0.0625F) < 1e-6F &&
                std::abs(audio[64u + frame] + 0.125F) < 1e-6F,
            "original-only Adaptive Prediction must apply its gain to both selected channels");
  }
  return failures == 0 ? 0 : 1;
}
