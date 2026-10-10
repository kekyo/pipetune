/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_audio_decoder.h"
#include "dsp_backend_loader.h"
#include "sfz/parser.h"
#include "sfz/bank.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iostream>
#include <iterator>
#include <numbers>

static void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

struct NativeEngine {
  const pipetune::DspBackendApi &api;
  et_engine handle;
  ~NativeEngine() { if (handle) api.engineDestroy(handle); }
};

int main(int argc, char **argv) {
  if (argc != 2) return 2;
  try {
    const auto original = std::vector<std::uint8_t>(std::istreambuf_iterator<char>(std::cin), {});
    auto samples = std::vector<pipetune::assets::SfzSample>{};
    samples.push_back({"instrument.wav", pipetune::decodeAssetAudio(original, 64 * 1024 * 1024)});
    const auto documents = std::vector<pipetune::assets::SfzDocument>{{"main.sfz", argv[1]}};
    const auto parsed = pipetune::assets::parseSfz("main.sfz", documents, std::nullopt, 256 * 1024 * 1024);
    const auto bank = pipetune::assets::packSfzBank(parsed.regions, samples, 256 * 1024 * 1024);
    const auto backend = pipetune::loadDspBackend(pipetune::DspBackendKind::scalar);
    require(backend.backend != nullptr, backend.error.c_str());
    const auto &api = pipetune::dspBackendApi(*backend.backend);
    const auto engine = NativeEngine{api, api.engineCreate()};
    require(engine.handle != 0 && api.enginePrepare(engine.handle, 48000, 2, 128, 0) == ET_OK,
            "cannot prepare the native SFZ engine");
    const auto *definition = pipetune::findDspByDisplayName("SFZ Note Player");
    require(definition != nullptr, "SFZ Note Player is absent from the catalog");
    const auto parameters = std::string(R"({"th":0.01,"mn":83,"mx":83,"dm":0,"wm":100})");
    const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
        yyjson_read(parameters.data(), parameters.size(), 0), yyjson_doc_free);
    const auto packed = pipetune::packDspParameters(*definition, yyjson_doc_get_root(document.get()));
    require(packed.error.empty(), packed.error.c_str());
    const auto instance = api.instanceCreate(engine.handle, definition->typeName.data());
    require(instance != 0 && api.instanceSetParams(engine.handle, instance, packed.floats.data(),
        packed.floats.size(), definition->hash, 0) == ET_OK, "cannot configure SFZ playback");
    const auto info = pipetune_effetune_asset_info_v1{.channels = 1, .frames = bank.samples,
        .topology = 0, .head_block = 0, .rate_divider = 1, .path_count = 0, .input_count = 0,
        .processing_channels = 1, .footprint_bytes = static_cast<std::uint32_t>(bank.footprintBytes),
        .byte_size = static_cast<std::uint32_t>(bank.payload.size())};
    require(api.instanceAssetCopy(engine.handle, instance, 0, &info, bank.payload.data(),
        bank.payload.size(), ET_ASSET_F32_MULTICH) == ET_OK, "native SFZ rejected the generated bank");
    auto block = std::vector<float>(256);
    for (auto frames = 0u; frames < bank.warmupFrames + 256 &&
        api.instanceAssetState(engine.handle, instance, 0) == ET_ASSET_STATE_PREPARING; frames += 128) {
      std::fill(block.begin(), block.end(), 0.0F);
      require(api.instanceProcess(engine.handle, instance, block.data(), 2, 128, frames / 48000.0) == ET_OK,
              "SFZ warmup failed");
    }
    require(api.instanceAssetState(engine.handle, instance, 0) == ET_ASSET_STATE_ACTIVE &&
        api.instanceReset(engine.handle, instance) == ET_OK, "SFZ bank did not become active");
    constexpr auto totalFrames = 24000u;
    auto output = std::vector<float>(2 * totalFrames);
    for (auto offset = 0u; offset < totalFrames; offset += 128) {
      const auto count = std::min(128u, totalFrames - offset);
      for (auto frame = 0u; frame < count; ++frame) {
        const auto value = static_cast<float>(0.3 * std::sin(2 * std::numbers::pi * 1000 * (offset + frame) / 48000.0));
        block[frame] = block[count + frame] = value;
      }
      require(api.instanceProcess(engine.handle, instance, block.data(), 2, count, offset / 48000.0) == ET_OK,
              "SFZ audio processing failed");
      for (auto channel = 0u; channel < 2; ++channel)
        std::copy_n(block.begin() + channel * count, count, output.begin() + channel * totalFrames + offset);
    }
    std::cout << "{\"latencyFrames\":" << api.instanceLatency(engine.handle, instance) << ",\"assetActive\":true}\n";
    for (const auto sample : output) {
      const auto bits = std::bit_cast<std::uint32_t>(sample);
      for (auto byte = 0u; byte < 4; ++byte) std::cout.put(static_cast<char>(bits >> (8 * byte)));
    }
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
