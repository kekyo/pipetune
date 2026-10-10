/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "room-eq/design.h"

#include <bit>
#include <cmath>
#include <iostream>
#include <iomanip>
#include <stdexcept>

static std::uint64_t readWord(unsigned bytes) {
  auto word = std::uint64_t{0};
  for (auto byte = 0u; byte < bytes; ++byte) {
    const auto value = std::cin.get();
    if (value == std::char_traits<char>::eof()) throw std::invalid_argument("truncated design request");
    word |= static_cast<std::uint64_t>(value) << (byte * 8);
  }
  return word;
}

static double readDouble() { return std::bit_cast<double>(readWord(8)); }

static std::uint32_t readCount(std::uint32_t maximum) {
  const auto value = readWord(4);
  if (value > maximum) throw std::invalid_argument("oversized design request");
  return value;
}

static void printDiagnostics(const std::vector<pipetune::assets::RoomEqDiagnostic> &diagnostics) {
  std::cout << '[';
  for (auto index = std::size_t{0}; index < diagnostics.size(); ++index) {
    if (index) std::cout << ',';
    const auto &value = diagnostics[index];
    std::cout << "{\"state\":\"" << value.state << "\",\"reason\":";
    if (value.reason) std::cout << '"' << *value.reason << '"'; else std::cout << "null";
    if (value.scale) std::cout << ",\"scale\":" << *value.scale;
    if (value.effectiveWindowMs) std::cout << ",\"effectiveWindowMs\":" << *value.effectiveWindowMs;
    if (value.agreementMinimum) std::cout << ",\"agreementMinimum\":" << *value.agreementMinimum;
    if (value.residualMaximumMs) std::cout << ",\"residualMaximumMs\":" << *value.residualMaximumMs;
    std::cout << '}';
  }
  std::cout << ']';
}

static int designInput(bool impulses, bool phase) {
  using namespace pipetune::assets;
  auto config = RoomEqConfiguration{};
  config.sampleRate = readCount(768000); config.taps = readCount(131072);
  config.phase = static_cast<RoomEqPhase>(readCount(2));
  config.smoothing = readDouble(); config.lowFrequency = readDouble();
  config.highFrequency = readDouble(); config.maxBoostDb = readDouble();
  config.correctionAmount = readDouble();
  if (phase) {
    config.directWindowMs = readDouble();
    const auto phaseLow = readDouble();
    if (phaseLow >= 0) config.phaseLowFrequency = phaseLow;
    config.phaseCorrectionAmount = readDouble(); config.reverbAmount = readDouble();
    config.reverbWindowMs = readDouble(); config.reverbMaxFrequency = readDouble();
    config.reverbSmoothing = readDouble();
    const auto phaseSmoothing = readDouble();
    if (phaseSmoothing >= 0) config.phaseSmoothing = phaseSmoothing;
    config.lowFrequencyPhaseExtension = readCount(1) != 0;
    config.referencePoint = readWord(8);
  }
  config.eqBands.resize(readCount(64));
  for (auto &band : config.eqBands) {
    band.type = static_cast<RoomEqBandType>(readCount(2));
    band.enabled = readCount(1) != 0;
    band.frequency = readDouble(); band.gain = readDouble(); band.q = readDouble();
  }
  auto sources = std::vector<RoomEqSource>(readCount(16));
  for (auto &source : sources) {
    source.assigned = readCount(1) != 0;
    source.response.resize(readCount(100000));
    for (auto &point : source.response) { point.frequency = readDouble(); point.decibels = readDouble(); }
    if (impulses) {
      source.impulses.resize(readCount(10000));
      for (auto &impulse : source.impulses) {
        impulse.pointId = readCount(UINT32_MAX); impulse.sampleRate = readCount(768000);
        impulse.onsetIndex = readCount(UINT32_MAX); impulse.referenceScale = readDouble();
        impulse.samples.resize(readCount(16 * 1024 * 1024));
        for (auto &sample : impulse.samples) sample = std::bit_cast<float>(static_cast<std::uint32_t>(readWord(4)));
      }
    }
  }
  if (std::cin.peek() != std::char_traits<char>::eof()) throw std::invalid_argument("trailing design request data");
  const auto result = designRoomEq(config, sources);
  std::cout << "{\"filterDelaySamples\":" << result.filterDelaySamples
            << ",\"supportsFullPhase\":" << (result.supportsFullPhase ? "true" : "false")
            << ",\"qualityWarnings\":[";
  for (auto index = std::size_t{0}; index < result.qualityWarnings.size(); ++index) {
    if (index) std::cout << ',';
    std::cout << '"' << result.qualityWarnings[index] << '"';
  }
  std::cout << std::setprecision(17) << "],\"diagnostics\":{\"phaseCorrection\":";
  printDiagnostics(result.diagnostics.phaseCorrection);
  std::cout << ",\"lowFrequencyPhaseExtension\":";
  printDiagnostics(result.diagnostics.lowFrequencyPhaseExtension);
  std::cout << ",\"reverbCorrection\":";
  printDiagnostics(result.diagnostics.reverbCorrection);
  std::cout << "}}\n";
  for (const auto &channel : result.channels)
    for (const auto sample : channel) {
      const auto word = std::bit_cast<std::uint32_t>(sample);
      for (auto byte = 0u; byte < 4; ++byte) std::cout.put(static_cast<char>((word >> (byte * 8)) & 255));
    }
  return 0;
}

int main(int argc, char **argv) {
  try {
    if (argc == 2 && std::string_view(argv[1]) == "--design") return designInput(false, false);
    if (argc == 2 && std::string_view(argv[1]) == "--design-ir") return designInput(true, false);
    if (argc == 2 && std::string_view(argv[1]) == "--design-phase") return designInput(true, true);
    if (argc != 1) return 2;
    using namespace pipetune::assets;
    auto config = RoomEqConfiguration{};
    config.taps = 8192;
    const auto sources = std::vector<RoomEqSource>{{true, {{20, 4}, {1000, 4}, {20000, 4}}}, {}};
    for (const auto phase : {RoomEqPhase::Minimum, RoomEqPhase::Linear}) {
      config.phase = phase;
      const auto designed = designRoomEq(config, sources);
      const auto delay = phase == RoomEqPhase::Minimum ? 0u : config.taps / 2;
      if (designed.filterDelaySamples != delay || designed.channels.size() != 2) return 1;
      for (const auto &channel : designed.channels)
        for (auto frame = std::size_t{0}; frame < channel.size(); ++frame)
          if (std::abs(channel[frame] - (frame == delay ? 1.0 : 0.0)) > 2e-7) return 1;
    }
    std::cout << "Room EQ minimum/linear flat and unassigned responses passed\n";
    return 0;
  } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}
