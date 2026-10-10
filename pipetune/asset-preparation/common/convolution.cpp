/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "convolution.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>

namespace pipetune::assets {

// Ported from EffeTune v2.13.0 js/ir-library/ir-plugin-contract.js (MIT).
static std::uint64_t powerOfTwo(std::uint64_t value) {
  return std::bit_ceil(std::max(std::uint64_t{1}, value));
}

static std::uint32_t matrixInputs(const ConvolutionConfig &config) {
  auto inputs = std::set<std::uint32_t>{};
  for (const auto &path : config.paths) inputs.insert(path.input);
  return static_cast<std::uint32_t>(inputs.size());
}

std::uint64_t convolutionFootprint(std::uint32_t frames, std::uint32_t channels,
                                  const ConvolutionConfig &config) {
  const auto width = config.processingChannels;
  const auto paths = config.topology == Topology::mono ? width :
                     config.topology == Topology::trueStereo ? 4u :
                     config.topology == Topology::matrix ? config.paths.size() : channels;
  const auto inputs = config.topology == Topology::matrix ? matrixInputs(config) : width;
  const auto latency = config.headBlock;
  const auto head = latency == 0u ? 128u : latency;
  auto requiredRing = std::uint64_t{latency} + 4096u;
  auto convolver = std::uint64_t{16u * 1024u};
  const auto addStage = [&](std::uint32_t block, std::uint32_t offset, std::uint32_t end) {
    if (offset >= frames || end <= offset) return;
    const auto segment = static_cast<std::uint64_t>(std::min(end, frames) - offset);
    requiredRing = std::max(requiredRing, static_cast<std::uint64_t>(latency) + offset + block + 4096u);
    const auto fft = static_cast<std::uint64_t>(block) * 2u;
    const auto partitions = (segment + block - 1u) / block;
    const auto floats = 3u * inputs * block + 2u * fft +
                        (inputs + channels) * partitions * fft + 2u * width * fft;
    convolver += 512u + floats * 4u + powerOfTwo(paths) * 12u + 136u + fft * 4u;
  };
  addStage(head, latency == 0u ? 128u : 0u, 4u * head);
  for (auto block = 2u * head; block < 4096u; block *= 2u) addStage(block, 2u * block, 4u * block);
  addStage(4096u, 8192u, frames);
  convolver += width * powerOfTwo(requiredRing) * 4u;
  if (latency == 0u) convolver += (channels + inputs) * 128u * 4u;
  convolver += inputs * 4u;
  const auto payload = 32u + config.paths.size() * 12u + static_cast<std::uint64_t>(frames) * channels * 4u;
  return std::max(payload + convolver,
                  payload + static_cast<std::uint64_t>(frames) * channels * 16u + 2u * 1024u * 1024u);
}

static void appendWord(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
  for (auto shift = 0u; shift < 32u; shift += 8u) bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

ConvolutionAsset encodeConvolution(const Audio &audio, const ConvolutionConfig &config) {
  const auto channels = audio.channels.size();
  const auto width = config.processingChannels;
  const auto head = config.headBlock;
  if (channels == 0 || channels > 16 || width == 0 || width > 16 ||
      audio.channels.front().empty() || audio.channels.front().size() > UINT32_MAX ||
      audio.sampleRate == 0 || config.paths.size() > 16 ||
      (head != 0 && head != 128 && head != 256 && head != 512 && head != 1024) ||
      (config.rateDivider != 1 && config.rateDivider != 2 && config.rateDivider != 4) ||
      (head == 0 && config.rateDivider != 1) ||
      (config.topology == Topology::mono && channels != 1) ||
      (config.topology == Topology::independent && channels != width) ||
      (config.topology == Topology::trueStereo && (channels != 4 || width != 2)) ||
      (config.topology == Topology::matrix) != !config.paths.empty())
    throw std::invalid_argument("invalid convolution configuration");
  const auto frames = static_cast<std::uint32_t>(audio.channels.front().size());
  for (const auto &channel : audio.channels) {
    if (channel.size() != frames) throw std::invalid_argument("IR channels have different lengths");
    for (const auto sample : channel)
      if (!std::isfinite(sample)) throw std::invalid_argument("IR contains non-finite PCM");
  }
  for (const auto &path : config.paths)
    if (path.input >= width || path.output >= width || path.response >= channels)
      throw std::invalid_argument("IR route is outside the selected channels");
  const auto footprint = convolutionFootprint(frames, channels, config);
  if (footprint > 32u * 1024u * 1024u) throw std::length_error("IR exceeds the 32 MiB convolution budget");
  auto result = ConvolutionAsset{.config = config,
      .channels = static_cast<std::uint32_t>(channels), .frames = frames,
      .sampleRate = audio.sampleRate, .inputCount = matrixInputs(config),
      .footprintBytes = footprint, .payload = {}};
  result.payload.reserve(32u + config.paths.size() * 12u + channels * frames * 4u);
  for (const auto word : {0x31415445u, result.channels, frames, audio.sampleRate,
                          static_cast<std::uint32_t>(config.topology),
                          static_cast<std::uint32_t>(config.paths.size()), 0u, 0u})
    appendWord(result.payload, word);
  for (const auto &path : config.paths) {
    appendWord(result.payload, path.input);
    appendWord(result.payload, path.output);
    appendWord(result.payload, path.response);
  }
  for (const auto &channel : audio.channels)
    for (const auto sample : channel) appendWord(result.payload, std::bit_cast<std::uint32_t>(sample));
  return result;
}

} // namespace pipetune::assets
