/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "ir/preparation.h"

#include <bit>
#include <cmath>
#include <iomanip>
#include <iostream>

static void printAnalysis(const pipetune::assets::IrAnalysis &analysis) {
  std::cout << "{\"rt60Seconds\":";
  if (analysis.rt60Seconds) std::cout << *analysis.rt60Seconds;
  else std::cout << "null";
  std::cout << ",\"peakDb\":" << analysis.peakDb << ",\"l1GainUpperBound\":" << analysis.l1GainUpperBound;
  const auto printArray = [](const char *name, const auto &values) {
    std::cout << ",\"" << name << "\":[";
    for (auto index = std::size_t{0}; index < values.size(); ++index) {
      if (index != 0) std::cout << ',';
      std::cout << values[index];
    }
    std::cout << ']';
  };
  printArray("sampleFrames", analysis.sampleFrames);
  printArray("envelope", analysis.envelope);
  printArray("edcDb", analysis.edcDb);
  std::cout << '}';
}

static int prepareInput(char **argv) {
  using namespace pipetune::assets;
  const auto rate = static_cast<std::uint32_t>(std::stoul(argv[2]));
  const auto channels = static_cast<std::uint32_t>(std::stoul(argv[3]));
  const auto frames = static_cast<std::uint32_t>(std::stoul(argv[4]));
  const auto width = static_cast<std::uint32_t>(std::stoul(argv[5]));
  const auto head = static_cast<std::uint32_t>(std::stoul(argv[7]));
  const auto configuration = resolveIrConfiguration(rate, channels, width, argv[6], head, argv[8]);
  if (channels == 0 || frames == 0 || static_cast<std::uint64_t>(channels) * frames > 16u * 1024u * 1024u) return 2;
  auto audio = Audio{.sampleRate = configuration.sampleRate,
                     .channels = std::vector<std::vector<float>>(channels, std::vector<float>(frames))};
  for (auto &channel : audio.channels)
    for (auto &sample : channel) {
      auto word = std::uint32_t{0};
      for (auto byte = 0u; byte < 4u; ++byte) {
        const auto value = std::cin.get();
        if (value == std::char_traits<char>::eof()) return 2;
        word |= static_cast<std::uint32_t>(value) << (8u * byte);
      }
      sample = std::bit_cast<float>(word);
    }
  const auto prepared = prepareIr(audio, configuration,
      {.directCut = std::string_view(argv[9]) == "1", .cutOffsetMs = std::stod(argv[10]),
       .decayPercent = std::stod(argv[11]), .trimPercent = std::stod(argv[12])});
  std::cout << std::setprecision(17) << "{\"onsetFrame\":" << prepared.onsetFrame
            << ",\"leadingSilenceFrames\":" << prepared.leadingSilenceFrames
            << ",\"sourceStartFrame\":" << prepared.sourceStartFrame
            << ",\"footprintBytes\":" << prepared.asset.footprintBytes
            << ",\"capacityLimited\":" << (prepared.capacityLimited ? "true" : "false")
            << ",\"original\":";
  printAnalysis(prepared.original);
  std::cout << ",\"analysis\":";
  printAnalysis(prepared.analysis);
  for (const auto &entry : {std::pair{"initialGains", &prepared.initialGains}, std::pair{"finalGains", &prepared.finalGains}}) {
    std::cout << ",\"" << entry.first << "\":[";
    for (auto channel = std::size_t{0}; channel < entry.second->size(); ++channel) {
      if (channel != 0) std::cout << ',';
      std::cout << (*entry.second)[channel];
    }
    std::cout << ']';
  }
  std::cout << "}\n";
  std::cout.write(reinterpret_cast<const char *>(prepared.asset.payload.data()), prepared.asset.payload.size());
  return 0;
}

int main(int argc, char **argv) {
  if (argc == 13 && std::string_view(argv[1]) == "--prepare") {
    try { return prepareInput(argv); }
    catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
  }
  auto original = pipetune::assets::Audio{.sampleRate = 48000, .channels = {std::vector<float>(512)}};
  const auto silent = argc >= 3 && std::string_view(argv[2]) == "silence";
  if (!silent) {
    original.channels[0][0] = 1.0F;
    original.channels[0][64] = 0.5F;
  }
  const auto configuration = pipetune::assets::resolveIrConfiguration(48000, 1, 2, "auto", 128, "full");
  const auto prepared = pipetune::assets::prepareIr(original, configuration, {}).asset;
  if (prepared.frames != 512 || prepared.channels != 1 || prepared.payload.size() != 32 + 512 * 4) return 1;
  const auto sampleAt = [&](std::size_t frame) {
    auto word = std::uint32_t{0};
    for (auto byte = 0u; byte < 4u; ++byte) word |= static_cast<std::uint32_t>(prepared.payload[32 + frame * 4 + byte]) << (byte * 8u);
    return std::bit_cast<float>(word);
  };
  for (auto frame = 0u; frame < 512u; ++frame) {
    const auto expected = silent ? 0 : frame == 0 ? 1.0 / std::sqrt(1.25) : frame == 64 ? 0.5 / std::sqrt(1.25) : 0;
    if (std::abs(sampleAt(frame) - expected) > 1e-7) return 2;
  }
  if (argc >= 2 && std::string_view(argv[1]) == "--payload") {
    std::cout.write(reinterpret_cast<const char *>(prepared.payload.data()), prepared.payload.size());
    return 0;
  }
  std::cout << "mono IR: 512 frames, energy 1, footprint " << prepared.footprintBytes << '\n';
  return 0;
}
