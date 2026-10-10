/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "preparation.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numeric>
#include <numbers>
#include <stdexcept>

namespace pipetune::assets {

// Adapted from EffeTune v2.13.0 ir-plugin-contract.js and ir-preparation.js.
// Copyright (c) 2025-2026 Yoshiyuki Kobayashi. MIT; see packaging/copyright.
IrConfiguration resolveIrConfiguration(std::uint32_t sampleRate, std::uint32_t channelCount,
    std::uint32_t processingChannels, std::string_view channelMode,
    std::uint32_t headBlock, std::string_view convolutionRate) {
  if (sampleRate == 0 || channelCount < 1 || channelCount > 16 ||
      processingChannels < 1 || processingChannels > 16)
    throw std::invalid_argument("IR source or selected audio channels are unavailable");
  if (headBlock != 0 && headBlock != 128 && headBlock != 256 && headBlock != 512 && headBlock != 1024)
    throw std::invalid_argument("IR latency must be 0, 128, 256, 512, or 1024");
  if (headBlock == 0) convolutionRate = "full";
  if (convolutionRate == "auto") convolutionRate = sampleRate >= 88200 ? "half" : "full";
  if (convolutionRate != "full" && convolutionRate != "half" && convolutionRate != "quarter")
    throw std::invalid_argument("unsupported IR convolution rate");
  if (convolutionRate == "quarter" && sampleRate < 176400)
    throw std::invalid_argument("quarter-rate IR requires at least 176.4 kHz");
  const auto divider = convolutionRate == "quarter" ? 4u : convolutionRate == "half" ? 2u : 1u;
  if (channelMode == "auto")
    channelMode = channelCount == 1 ? "mono" : channelCount == 4 && processingChannels == 2 ? "true" :
                  channelCount == processingChannels ? "indep" : "multi";
  auto config = ConvolutionConfig{.headBlock = headBlock, .rateDivider = divider,
                                  .processingChannels = processingChannels, .paths = {}};
  auto channels = channelCount;
  if (channelMode == "mono") {
    config.topology = Topology::mono;
    channels = 1;
  } else if (channelMode == "true") {
    if (channelCount != 4 || processingChannels != 2)
      throw std::invalid_argument("true stereo requires a four-channel IR and two selected channels");
    config.topology = Topology::trueStereo;
  } else if (channelMode == "indep") {
    if (channelCount < processingChannels)
      throw std::invalid_argument("independent mode requires one IR per selected channel");
    config.topology = Topology::independent;
    channels = processingChannels;
  } else if (channelMode == "multi") {
    config.topology = Topology::matrix;
    for (auto channel = 0u; channel < std::min(channelCount, processingChannels); ++channel)
      config.paths.push_back({channel, channel, channel});
  } else {
    throw std::invalid_argument("unsupported IR channel mode");
  }
  return {.convolution = std::move(config), .assetChannels = channels,
          .sampleRate = static_cast<std::uint32_t>(std::floor(static_cast<double>(sampleRate) / divider + 0.5))};
}

static std::vector<double> frameEnergies(const Audio &audio, const std::vector<float> &gains) {
  auto energies = std::vector<double>(audio.channels.front().size());
  for (auto channel = std::size_t{0}; channel < audio.channels.size(); ++channel) {
    const auto gain = gains.empty() ? 1.0 : gains[channel];
    for (auto frame = std::size_t{0}; frame < energies.size(); ++frame) {
      const auto sample = audio.channels[channel][frame] * gain;
      energies[frame] += sample * sample;
    }
  }
  return energies;
}

static std::vector<float> gainsFromEnergies(const std::vector<double> &energies, Topology topology) {
  auto gains = std::vector<float>(energies.size(), 1.0F);
  if (topology == Topology::trueStereo) {
    const auto energy = std::accumulate(energies.begin(), energies.end(), 0.0);
    std::fill(gains.begin(), gains.end(), static_cast<float>(energy > 0 ? std::sqrt(energies.size() / energy) : 1));
  } else {
    for (auto channel = std::size_t{0}; channel < energies.size(); ++channel)
      gains[channel] = static_cast<float>(energies[channel] > 0 ? 1.0 / std::sqrt(energies[channel]) : 1);
  }
  return gains;
}

static std::vector<float> normalizationGains(const Audio &audio, Topology topology) {
  auto energies = std::vector<double>(audio.channels.size());
  for (auto channel = std::size_t{0}; channel < audio.channels.size(); ++channel)
    for (const auto sample : audio.channels[channel]) energies[channel] += static_cast<double>(sample) * sample;
  return gainsFromEnergies(energies, topology);
}

static void applyGains(Audio &audio, const std::vector<float> &gains) {
  for (auto channel = std::size_t{0}; channel < audio.channels.size(); ++channel)
    if (gains[channel] != 1.0F)
      for (auto &sample : audio.channels[channel]) sample *= gains[channel];
}

static std::vector<double> energyDecay(const Audio &audio, const std::vector<float> &gains) {
  auto accumulated = frameEnergies(audio, gains);
  auto total = 0.0;
  for (auto frame = accumulated.size(); frame-- > 0;) {
    total += accumulated[frame];
    accumulated[frame] = total;
  }
  return accumulated;
}

static std::optional<double> estimateRt60(const Audio &audio, const std::vector<float> &gains) {
  const auto accumulated = energyDecay(audio, gains);
  const auto total = accumulated.front();
  if (!(total > 0)) return std::nullopt;
  auto count = 0.0;
  auto sumTime = 0.0;
  auto sumDb = 0.0;
  auto sumTimeDb = 0.0;
  auto sumTimeSquared = 0.0;
  for (auto frame = std::size_t{0}; frame < accumulated.size(); ++frame) {
    const auto db = 10 * std::log10(accumulated[frame] / total);
    if (db > -5 || db < -35) continue;
    const auto time = static_cast<double>(frame) / audio.sampleRate;
    ++count;
    sumTime += time;
    sumDb += db;
    sumTimeDb += time * db;
    sumTimeSquared += time * time;
  }
  const auto denominator = count * sumTimeSquared - sumTime * sumTime;
  if (count < 8 || denominator == 0) return std::nullopt;
  const auto slope = (count * sumTimeDb - sumTime * sumDb) / denominator;
  return slope < 0 ? std::optional<double>(-60 / slope) : std::nullopt;
}

struct DecayShape {
  double slope = 0;
  double offset = 0;
};

static DecayShape decayShape(std::size_t frames, std::uint32_t sampleRate,
                             double decayPercent, const std::optional<double> &rt60) {
  if (decayPercent == 100 || !rt60 || !(*rt60 > 0)) return {};
  const auto slope = (-60 / *rt60) * (100 / decayPercent - 1);
  return {.slope = slope, .offset = std::max(0.0, slope * (frames - 1) / sampleRate)};
}

static double decayGain(const DecayShape &shape, std::size_t frame, std::uint32_t sampleRate) {
  return std::pow(10.0, (shape.slope * frame / sampleRate - shape.offset) / 20);
}

static double fadeOutGain(std::size_t index, std::size_t frames) {
  const auto phase = frames == 1 ? 1.0 : static_cast<double>(index) / (frames - 1);
  return 0.5 + 0.5 * std::cos(std::numbers::pi * phase);
}

static void fadeOut(Audio &audio) {
  const auto frames = std::min(std::size_t{2048}, audio.channels.front().size());
  const auto start = audio.channels.front().size() - frames;
  for (auto frame = std::size_t{0}; frame < frames; ++frame) {
    const auto gain = fadeOutGain(frame, frames);
    for (auto &channel : audio.channels) channel[start + frame] = static_cast<float>(channel[start + frame] * gain);
  }
}

static std::size_t outputFrameCount(std::size_t frames, double trimPercent) {
  return std::max(std::size_t{1}, static_cast<std::size_t>(std::floor(frames * trimPercent / 100 + 0.5)));
}

static Audio sliced(const Audio &audio, std::size_t start) {
  auto result = Audio{.sampleRate = audio.sampleRate, .channels = {}};
  for (const auto &channel : audio.channels) result.channels.emplace_back(channel.begin() + start, channel.end());
  return result;
}

static IrAnalysis analyze(const Audio &audio, const ConvolutionConfig &config) {
  auto result = IrAnalysis{};
  const auto frames = audio.channels.front().size();
  const auto count = std::min(std::size_t{1600}, frames);
  const auto accumulated = energyDecay(audio, {});
  const auto total = accumulated.front();
  auto bounds = std::vector<double>(audio.channels.size());
  auto peak = 0.0;
  for (auto channel = std::size_t{0}; channel < audio.channels.size(); ++channel)
    for (const auto sample : audio.channels[channel]) {
      const auto magnitude = std::abs(static_cast<double>(sample));
      peak = std::max(peak, magnitude);
      bounds[channel] += magnitude;
    }
  if (config.topology == Topology::mono) result.l1GainUpperBound = bounds.front();
  else if (config.topology == Topology::trueStereo)
    result.l1GainUpperBound = std::max(bounds[0] + bounds[2], bounds[1] + bounds[3]);
  else if (config.topology == Topology::matrix) {
    auto outputs = std::array<double, 16>{};
    for (const auto &path : config.paths) outputs.at(path.output) += bounds.at(path.response);
    result.l1GainUpperBound = *std::max_element(outputs.begin(), outputs.end());
  } else result.l1GainUpperBound = *std::max_element(bounds.begin(), bounds.end());
  for (auto point = std::size_t{0}; point < count; ++point) {
    const auto start = point * frames / count;
    const auto end = std::max(start + 1, (point + 1) * frames / count);
    auto binPeak = 0.0F;
    for (const auto &channel : audio.channels)
      for (auto frame = start; frame < end; ++frame) binPeak = std::max(binPeak, std::abs(channel[frame]));
    result.sampleFrames.push_back(start);
    result.envelope.push_back(binPeak);
    const auto ratio = total > 0 ? accumulated[start] / total : 0;
    result.edcDb.push_back(static_cast<float>(ratio > 0 ? std::max(-120.0, 10 * std::log10(ratio)) : -120));
  }
  result.peakDb = peak > 0 ? 20 * std::log10(peak) : -120;
  result.rt60Seconds = estimateRt60(audio, {});
  return result;
}

PreparedIr prepareIr(const Audio &audio, const IrConfiguration &configuration, const IrOptions &options) {
  const auto &config = configuration.convolution;
  if (audio.sampleRate != configuration.sampleRate || audio.sampleRate == 0 ||
      audio.channels.empty() || audio.channels.size() > 16 || audio.channels.front().empty() ||
      configuration.assetChannels == 0 || configuration.assetChannels > audio.channels.size() ||
      (config.topology == Topology::trueStereo && audio.channels.size() != 4) ||
      !std::isfinite(options.cutOffsetMs) || options.cutOffsetMs < -20 || options.cutOffsetMs > 50 ||
      !std::isfinite(options.decayPercent) || options.decayPercent < 10 || options.decayPercent > 400 ||
      !std::isfinite(options.trimPercent) || options.trimPercent < 1 || options.trimPercent > 100)
    throw std::invalid_argument("IR preparation input or options are outside supported ranges");
  const auto frames = audio.channels.front().size();
  for (const auto &channel : audio.channels) {
    if (channel.size() != frames) throw std::invalid_argument("IR channels have different lengths");
    for (const auto sample : channel)
      if (!std::isfinite(sample)) throw std::invalid_argument("IR contains non-finite PCM");
  }
  auto result = PreparedIr{};
  const auto energies = frameEnergies(audio, {});
  auto leading = std::size_t{0};
  while (leading < frames && energies[leading] <= 1e-20) ++leading;
  auto onset = leading;
  if (leading == frames) leading = onset = 0;
  else {
    const auto window = std::max(std::size_t{8}, static_cast<std::size_t>(std::floor(audio.sampleRate * 0.001 + 0.5)));
    auto windows = std::vector<double>(frames);
    auto running = 0.0;
    auto peak = 0.0;
    for (auto frame = std::size_t{0}; frame < frames; ++frame) {
      running += energies[frame];
      if (frame >= window) running -= energies[frame - window];
      windows[frame] = running;
      peak = std::max(peak, running);
    }
    while (onset < frames && windows[onset] < peak * 0.01) ++onset;
    if (onset == frames) onset = leading;
  }
  auto start = leading;
  if (options.directCut) {
    const auto cutOffset = static_cast<std::int64_t>(std::floor(options.cutOffsetMs * audio.sampleRate / 1000 + 0.5));
    start = static_cast<std::size_t>(std::max(static_cast<std::int64_t>(start),
                                             static_cast<std::int64_t>(onset) + cutOffset + 1));
  }
  start = std::min(start, frames - 1);
  result.onsetFrame = onset;
  result.leadingSilenceFrames = leading;
  result.sourceStartFrame = start;
  auto prepared = sliced(audio, start);
  auto shape = DecayShape{};
  if (options.directCut) {
    const auto reference = sliced(audio, leading);
    result.initialGains = normalizationGains(reference, config.topology);
    shape = decayShape(reference.channels.front().size(), audio.sampleRate, options.decayPercent,
                        estimateRt60(reference, result.initialGains));
    const auto referenceFrames = outputFrameCount(reference.channels.front().size(), options.trimPercent);
    const auto truncated = referenceFrames < reference.channels.front().size();
    const auto fadeFrames = truncated ? std::min(std::size_t{2048}, referenceFrames) : 0;
    const auto fadeStart = referenceFrames - fadeFrames;
    auto referenceEnergies = std::vector<double>(audio.channels.size());
    for (auto frame = std::size_t{0}; frame < referenceFrames; ++frame) {
      const auto decay = decayGain(shape, frame, audio.sampleRate);
      const auto fade = truncated && frame >= fadeStart ? fadeOutGain(frame - fadeStart, fadeFrames) : 1;
      for (auto channel = std::size_t{0}; channel < audio.channels.size(); ++channel) {
        const auto sample = static_cast<double>(reference.channels[channel][frame]) *
                            result.initialGains[channel] * decay * fade;
        referenceEnergies[channel] += sample * sample;
      }
    }
    result.finalGains = gainsFromEnergies(referenceEnergies, config.topology);
    const auto fadeFramesIn = std::min(std::size_t{64}, prepared.channels.front().size());
    for (auto frame = std::size_t{0}; frame < fadeFramesIn; ++frame) {
      const auto phase = fadeFramesIn == 1 ? 1.0 : static_cast<double>(frame) / (fadeFramesIn - 1);
      const auto gain = 0.5 - 0.5 * std::cos(std::numbers::pi * phase);
      for (auto &channel : prepared.channels) channel[frame] = static_cast<float>(channel[frame] * gain);
    }
  } else result.initialGains = normalizationGains(prepared, config.topology);
  applyGains(prepared, result.initialGains);
  result.original = analyze(prepared, config);
  if (!options.directCut)
    shape = decayShape(prepared.channels.front().size(), audio.sampleRate, options.decayPercent, result.original.rt60Seconds);
  const auto decayOffset = options.directCut ? start - leading : 0;
  if (shape.slope != 0 || shape.offset != 0)
    for (auto frame = std::size_t{0}; frame < prepared.channels.front().size(); ++frame) {
      const auto gain = decayGain(shape, frame + decayOffset, audio.sampleRate);
      for (auto &channel : prepared.channels) channel[frame] = static_cast<float>(channel[frame] * gain);
    }
  const auto outputFrames = outputFrameCount(prepared.channels.front().size(), options.trimPercent);
  result.trimmed = outputFrames < prepared.channels.front().size();
  if (result.trimmed) {
    for (auto &channel : prepared.channels) channel.resize(outputFrames);
    fadeOut(prepared);
  }
  if (!options.directCut) result.finalGains = normalizationGains(prepared, config.topology);
  applyGains(prepared, result.finalGains);

  // Emission selects channels only after host preparation and does not renormalize a capacity fade.
  prepared.channels.resize(configuration.assetChannels);
  auto low = std::uint32_t{1};
  auto high = static_cast<std::uint32_t>(prepared.channels.front().size());
  while (low < high) {
    const auto middle = low + (high - low + 1u) / 2u;
    if (convolutionFootprint(middle, configuration.assetChannels, config) <= 32u * 1024u * 1024u) low = middle;
    else high = middle - 1u;
  }
  result.capacityLimited = low < prepared.channels.front().size();
  if (result.capacityLimited) {
    for (auto &channel : prepared.channels) channel.resize(low);
    fadeOut(prepared);
  }
  result.analysis = analyze(prepared, config);
  result.asset = encodeConvolution(prepared, config);
  return result;
}

} // namespace pipetune::assets
