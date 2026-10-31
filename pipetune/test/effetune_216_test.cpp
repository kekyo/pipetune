/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "dsp_catalog.h"
#include "dsp_backend_loader.h"
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

static void testAnalyzerParameters(const pipetune::DspBackendApi &api) {
  for (const auto rhythm : {true, false}) {
    const auto engine = api.engineCreate();
    if (!check(engine != 0, "analyzer engine must be created")) continue;
    if (check(api.enginePrepare(engine, 48000, 2, 64, 0) == ET_OK,
              "analyzer engine must prepare")) {
      const auto instance = api.instanceCreate(engine,
          rhythm ? "RhythmAnalyzerPlugin" : "NoteSpectrogramPlugin");
      if (check(instance != 0, "analyzer instance must be created")) {
        for (const auto enabled : {0.0F, 1.0F}) {
          const auto parameters = rhythm ? std::array{40.0F, 240.0F, 0.0F, enabled} :
              std::array{28.0F, 91.0F, 0.0F, enabled};
          check(api.instanceSetParams(engine, instance, parameters.data(), parameters.size(),
                                      rhythm ? 0xf854c585u : 0xb8518943u, 0) == ET_OK,
                "the shared backend must accept the 2.16 four-float analyzer parameters");
          auto audio = std::array<float, 128>{};
          audio.fill(0.125F);
          check(api.instanceProcess(engine, instance, audio.data(), 2, 64, 0) == ET_OK,
                "the shared backend must process the staged analyzer parameters");
          for (const auto sample : audio)
            check(sample == 0.125F, "analyzers without metronome clicks must preserve PCM");
        }
      }
    }
    api.engineDestroy(engine);
  }
}

static void testMatrixReset(const pipetune::DspBackendApi &api) {
  const auto *definition = pipetune::findDspByTypeName("MatrixPlugin");
  if (!check(definition != nullptr, "Matrix must be available")) return;
  for (const auto resetEngine : {false, true}) {
    for (const auto applyBeforeReset : {false, true}) {
      for (const auto empty : {false, true}) {
        const auto engine = api.engineCreate();
        if (!check(engine != 0, "Matrix reset engine must be created")) continue;
        if (!check(api.enginePrepare(engine, 48000, 4, 64, 0) == ET_OK,
                   "Matrix reset engine must prepare")) {
          api.engineDestroy(engine);
          continue;
        }
        const auto instance = api.instanceCreate(engine, "MatrixPlugin");
        if (!check(instance != 0, "Matrix reset instance must be created")) {
          api.engineDestroy(engine);
          continue;
        }
        const auto render = [&]() {
          auto audio = std::array{0.25F, 0.125F, 0.75F, 0.5F};
          check(api.instanceProcess(engine, instance, audio.data(), 4, 1, 0) == ET_OK,
                "Matrix must process after reset");
          return audio;
        };
        check(render() == std::array{0.25F, 0.125F, 0.0F, 0.0F},
              "a new Matrix instance must start with stereo routing");
        // Keep inputs 1/2 and mix both into output 3 (mx = "00021112").
        auto routes = std::array<std::uint8_t, 16>{
            1, 0, 4, 0, 0, 0, 0, 0, 2, 0, 1, 1, 0, 1, 2, 0};
        if (empty) routes[2] = 0;
        check(api.instanceSetParams(engine, instance, nullptr, 0, definition->hash, 0) == ET_OK &&
                  api.instanceSetParamBytes(engine, instance, routes.data(), empty ? 4 : routes.size(),
                                            definition->hash, 0) == ET_OK,
              "Matrix routes must be staged");
        const auto expected = empty ? std::array<float, 4>{} : std::array{0.25F, 0.125F, 0.375F, 0.0F};
        if (applyBeforeReset) check(render() == expected, "Matrix must apply staged routes");
        for (auto repeat = 0; repeat < 2; ++repeat) {
          check((resetEngine ? api.engineReset(engine) : api.instanceReset(engine, instance)) == ET_OK,
                "Matrix instance and engine resets must succeed");
          check(render() == expected,
                "Matrix reset must retain active and pending routes, including empty routing");
        }
        api.engineDestroy(engine);
      }
    }
  }
}

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  check(pipetune::effetuneVersion() == "2.16.0", "the embedded app version must be 2.16.0");
  check(pipetune::generatedDspCatalog().size() == 112, "the app catalog must contain 112 DSPs");
  struct Contract {
    const char *type;
    std::uint32_t hash;
    std::uint32_t floats;
    std::uint32_t assetBytes;
  };
  for (const auto &contract : std::array{
           Contract{"RhythmAnalyzerPlugin", 0xf854c585u, 4u, 0u},
           Contract{"AnalogMeterPlugin", 0xad3d461cu, 7u, 0u},
           Contract{"AdaptivePredictionEffectPlugin", 0xebd8a6f0u, 10u, 0u},
           Contract{"SFZNotePlayerPlugin", 0xdfb4bbc5u, 20u, 1073741824u},
           Contract{"CassetteArtifactsPlugin", 0x328491aeu, 13u, 0u},
           Contract{"NoteSpectrogramPlugin", 0xb8518943u, 4u, 0u}}) {
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
    const auto &api = pipetune::dspBackendApi(*backend);
    testAnalyzerParameters(api);
    testMatrixReset(api);
    const auto engine = api.engineCreate();
    if (check(engine != 0, "the backend must create an engine")) {
      if (check(api.enginePrepare(engine, 48000.0F, 2u, 64u, 0u) == ET_OK,
                "the backend must prepare its engine")) {
        const auto instance = api.instanceCreate(engine, "SFZNotePlayerPlugin");
        if (check(instance != 0, "the backend must create SFZ Note Player")) {
          const auto parameters = std::array<float, 20>{
              0.1F, -60, 0, 21, 108, 64, 100, 0, 0, 0, 1, 1, 1, 0, 40, 50, 0, -1, 100, 1};
          check(api.instanceSetParams(engine, instance, parameters.data(), parameters.size(),
                                      0xdfb4bbc5u, 0u) == ET_OK,
                "the backend must accept the 2.16 SFZ Event detection parameters");
        }
      }
      api.engineDestroy(engine);
    }
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
            "the minimal 2.16 preset must process PCM");
    }
    for (auto frame = 0u; frame < 64u; ++frame)
      check(std::abs(audio[frame] - 0.0625F) < 1e-6F &&
                std::abs(audio[64u + frame] + 0.125F) < 1e-6F,
            "original-only Adaptive Prediction must apply its gain to both selected channels");
  }
  return failures == 0 ? 0 : 1;
}
