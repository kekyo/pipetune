/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "bass_management_config.h"
#include "dsp_backend_loader.h"
#include "generated_fir_asset.h"
#include <pipetune/dsp_pipeline.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numbers>
#include <vector>

static bool check(bool condition, const std::string &message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static std::vector<float> readSamples(const char *path, std::size_t count) {
  auto samples = std::vector<float>(count);
  auto file = std::ifstream(path, std::ios::binary);
  file.read(reinterpret_cast<char *>(samples.data()), count * sizeof(float));
  if (!file) return {};
  return samples;
}

static std::uint32_t readWord(const std::vector<std::uint8_t> &bytes, std::size_t offset) {
  auto value = std::uint32_t{0};
  for (auto i = 0u; i < 4u; ++i) value |= std::uint32_t{bytes[offset + i]} << (i * 8u);
  return value;
}

// Compare actual rendered audio, including full-range delay, main/sub summing,
// LFE filtering, inversions and gains, against the independent JS design.
template <typename Process>
static bool verifyPcm(Process process, std::span<const float> expected,
                       float rate, unsigned width, unsigned frames, unsigned blockSize) {
  auto maximum = 0.0F;
  for (auto start = 0u; start < frames;) {
    const auto count = std::min(blockSize, frames - start);
    auto block = std::vector<float>(width * count, 0.0F);
    for (auto ch = 0u; ch < width; ++ch) {
      const auto onset = 17u + ch * 3u;
      if (onset >= start && onset < start + count) block[ch * count + onset - start] = (ch + 1u) * 0.05F;
    }
    if (!process(block, count, start / static_cast<double>(rate))) return false;
    for (auto ch = 0u; ch < width; ++ch) {
      for (auto i = 0u; i < count; ++i) {
        const auto actual = block[ch * count + i];
        const auto reference = expected[ch * frames + start + i];
        maximum = std::max(maximum, std::abs(actual - reference));
        if (!check(std::isfinite(actual) && std::abs(actual - reference) < 2.0e-5F,
            "Bass Management PCM differs at " + std::to_string(start + i) + "/" + std::to_string(ch) +
            ": " + std::to_string(actual) + " vs " + std::to_string(reference))) return false;
      }
    }
    start += count;
  }
  std::cout << "pcm_max=" << maximum << ' ';
  return true;
}

static bool verifyNative(const pipetune::DspBackendApi &api, et_engine engine,
                         const pipetune::PackedParameters &packed,
                         pipetune::GeneratedFirAsset &asset, float rate,
                         unsigned width, unsigned frames, std::span<const float> expected) {
  if (!check(api.enginePrepare(engine, rate, width, width == 16u ? 8192u : 257u, 0u) == ET_OK, "native engine prepare failed")) return false;
  const auto instance = api.instanceCreate(engine, "BassManagementPlugin");
  if (!check(instance != 0u && api.instanceSetParams(engine, instance, packed.floats.data(),
      packed.floats.size(), packed.definition->hash, 0u) == ET_OK, "native parameters failed")) return false;
  if (!check(api.instanceAssetCopy(engine, instance, 0u, &asset.info, asset.payload.data(),
      asset.payload.size(), asset.formatTag) == ET_OK, "native asset copy failed")) return false;
  // A successful copy owns its data but is still PREPARING, not yet audible.
  std::fill(asset.payload.begin(), asset.payload.end(), 0xffu);
  auto state = api.instanceAssetState(engine, instance, 0u) & 0xffu;
  if (!check(state == ET_ASSET_STATE_PREPARING, "copy success must precede ACTIVE")) return false;
  auto block = std::vector<float>(width * 128u, 0.0F);
  auto preparationFrames = 0u;
  while (state == ET_ASSET_STATE_PREPARING && preparationFrames < 1048576u) {
    std::fill(block.begin(), block.end(), 0.0F);
    if (api.instanceProcess(engine, instance, block.data(), width, 128u, 0.0) != ET_OK) return false;
    if (!check(std::ranges::all_of(block, [](float sample) { return sample == 0.0F; }), "preparation silence must remain silent")) return false;
    state = api.instanceAssetState(engine, instance, 0u) & 0xffu;
    preparationFrames += 128u;
  }
  if (!check(state == ET_ASSET_STATE_ACTIVE, "asset must finish preparation in a bounded frame count")) return false;
  std::cout << "active_after=" << preparationFrames << ' ';
  for (const auto blockSize : {257u, 63u}) {
    if (!check(api.instanceReset(engine, instance) == ET_OK, "ACTIVE reset failed")) return false;
    if (!verifyPcm([&](auto &audio, unsigned count, double time) {
      return api.instanceProcess(engine, instance, audio.data(), width, count, time) == ET_OK;
    }, expected, rate, width, frames, blockSize)) return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc != 8) return 2;
  const auto rate = std::stof(argv[4]);
  const auto width = static_cast<unsigned>(std::stoul(argv[5]));
  const auto frames = static_cast<unsigned>(std::stoul(argv[6]));
  const auto footprint = static_cast<unsigned>(std::stoul(argv[7]));
  auto *document = yyjson_read_file(argv[1], 0, nullptr, nullptr);
  if (!document) return 2;
  const auto *definition = pipetune::findDspByDisplayName("Bass Management");
  const auto packed = pipetune::packDspParameters(*definition, yyjson_obj_get(
      yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(document), "pipeline"), 0), "parameters"));
  yyjson_doc_free(document);
  const auto config = pipetune::decodeBassManagementConfig(packed, width);
  if (!check(packed.error.empty() && config.error.empty(), packed.error + config.error)) return 1;
  const auto expectedIr = readSamples(argv[2], config.lowpassInputs.size() * config.taps);
  const auto expected = readSamples(argv[3], width * frames);
  if (expectedIr.empty() || expected.empty()) return 2;
  for (const auto kind : {pipetune::DspBackendKind::scalar, pipetune::DspBackendKind::simd}) {
    const auto backend = pipetune::loadDspBackend(kind);
    if (!check(backend.backend != nullptr, backend.error)) return 1;
    const auto &api = pipetune::dspBackendApi(*backend.backend);
    auto asset = pipetune::designBassManagementAsset(config, rate, width == 16u ? 8192u : 257u, api);
    if (!check(asset.error.empty() && !asset.payload.empty(), asset.error) ||
        !check(asset.info.footprint_bytes == footprint, "native and official JS memory bounds must agree")) return 1;
    auto maximum = 0.0F;
    const auto offset = 32u + asset.info.path_count * 12u;
    for (auto i = std::size_t{0}; i < expectedIr.size(); ++i) {
      const auto sample = std::bit_cast<float>(readWord(asset.payload, offset + i * 4u));
      maximum = std::max(maximum, std::abs(sample - expectedIr[i]));
      if (!check(std::isfinite(sample) && std::abs(sample - expectedIr[i]) < 2.0e-7F,
                 "native lowpass coefficients differ from official JS")) return 1;
    }
    // Measure response from the native coefficients, including cutoff and both
    // stop/pass bands, rather than merely checking the serialized asset fields.
    for (auto ir = 0u; ir < asset.info.channels; ++ir) {
      const auto input = config.lowpassInputs[ir];
      for (const auto frequency : {0.0, double(config.frequencies[input]), 1000.0}) {
        auto real = 0.0, imaginary = 0.0, referenceReal = 0.0, referenceImaginary = 0.0;
        for (auto tap = 0u; tap < config.taps; ++tap) {
          const auto phase = 2.0 * std::numbers::pi * frequency * tap / rate;
          const auto sample = std::bit_cast<float>(readWord(asset.payload, offset + (ir * config.taps + tap) * 4u));
          real += sample * std::cos(phase); imaginary += sample * std::sin(phase);
          referenceReal += expectedIr[ir * config.taps + tap] * std::cos(phase);
          referenceImaginary += expectedIr[ir * config.taps + tap] * std::sin(phase);
        }
        if (!check(std::abs(std::hypot(real, imaginary) - std::hypot(referenceReal, referenceImaginary)) < 2.0e-5,
                   "native FIR frequency response differs from JS")) return 1;
      }
    }
    std::cout << "ir_max=" << maximum << " footprint=" << asset.info.footprint_bytes << ' ';
    const auto engine = api.engineCreate();
    const auto nativePassed = engine != 0u && verifyNative(api, engine, packed, asset, rate, width, frames, expected);
    api.engineDestroy(engine);
    if (!nativePassed) return 1;
    // Reject oversized working buffers before FFT/allocation or asset transfer.
    const auto excessive = pipetune::designBassManagementAsset(config, rate, 0xffffffffu, api);
    if (!check(excessive.payload.empty() && excessive.error.find("32 MiB") != std::string::npos,
               "Bass Management capacity must include its working buffers")) return 1;
    const auto initialBackend = pipetune::loadDspBackend(kind == pipetune::DspBackendKind::scalar ?
        pipetune::DspBackendKind::simd : pipetune::DspBackendKind::scalar);
    auto loaded = pipetune::loadDspPipeline(argv[1], {rate == 48000 ? 96000.0F : 48000.0F, width, 257u}, initialBackend.backend);
    if (!check(loaded.pipeline != nullptr, loaded.error)) return 1;
    auto rebuilt = pipetune::rebuildDspPipeline(*loaded.pipeline, {rate, width, 257u}, backend.backend);
    if (!check(rebuilt.pipeline != nullptr && rebuilt.warnings.empty(), rebuilt.error) ||
        !check(rebuilt.pipeline->latencyFrames() == config.taps / 2u + 128u, "rebuilt FIR latency differs")) return 1;
    loaded = std::move(rebuilt);
    if (kind == pipetune::DspBackendKind::scalar && config.lowpassInputs.size() == 16u && config.taps == 32768u) {
      // The largest FIR set is supported at the host's 8192-frame capacity,
      // and even at 65536 frames; larger audio work buffers can cross 32 MiB.
      const auto admitted = pipetune::rebuildDspPipeline(*loaded.pipeline, {rate, width, 65536u});
      if (!check(admitted.pipeline != nullptr, admitted.error)) return 1;
      const auto rejected = pipetune::rebuildDspPipeline(*loaded.pipeline, {rate, width, 98304u});
      if (!check(rejected.pipeline == nullptr && rejected.error.find("Bass Management") != std::string::npos &&
                     rejected.error.find("32 MiB") != std::string::npos,
                 "oversized rebuild must fail with a Bass Management capacity error: " + rejected.error)) return 1;
      // The retained source is still rendered and compared below after rejection.
    }
    auto block = std::vector<float>(width * 128u, 0.0F);
    for (auto processed = 0u; processed < 262144u; processed += 128u) {
      std::fill(block.begin(), block.end(), 0.0F);
      if (loaded.pipeline->process(block, width, 128u, processed / double(rate)) != pipetune::ProcessStatus::ok) return 1;
    }
    if (!verifyPcm([&](auto &audio, unsigned count, double time) {
      return loaded.pipeline->process(audio, width, count, time) == pipetune::ProcessStatus::ok;
    }, expected, rate, width, frames, 127u)) return 1;
  }
  std::cout << '\n';
  return 0;
}
