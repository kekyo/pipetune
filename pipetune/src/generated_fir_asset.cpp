/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "generated_fir_asset.h"

#include "dsp_backend_loader.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <numbers>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune {

static constexpr auto kAssetHeaderBytes = std::uint32_t{32u};
static constexpr auto kAssetMagic = std::uint32_t{0x31415445u};
static constexpr auto kAssetCapacity = std::uint64_t{32u * 1024u * 1024u};
static constexpr auto kMonoTopology = std::uint32_t{1u};
static constexpr auto kMatrixTopology = std::uint32_t{4u};
static constexpr auto kMinimumMagnitude = 1.0e-8;
static constexpr auto kMagnitudeEpsilon = 1.0e-12;

struct GeneratedFirFftPlan {
  const DspBackendApi *api = nullptr;
  et_design_fft *handle = nullptr;
  std::uint32_t size = 0u;

  GeneratedFirFftPlan(const DspBackendApi &requestedApi,
                      std::uint32_t requestedSize)
      : api(&requestedApi), handle(requestedApi.designFftCreate(requestedSize)),
        size(requestedSize) {}

  ~GeneratedFirFftPlan() {
    if (handle != nullptr) {
      api->designFftDestroy(handle);
    }
  }

  GeneratedFirFftPlan(const GeneratedFirFftPlan &) = delete;
  GeneratedFirFftPlan &operator=(const GeneratedFirFftPlan &) = delete;
};

struct GeneratedFirSpectrum {
  std::vector<double> real;
  std::vector<double> imaginary;
};

struct GeneratedFirPath {
  std::uint32_t input;
  std::uint32_t output;
  std::uint32_t channel;
};

static std::string assetIndexedKey(char prefix, std::size_t index) {
  auto key = std::string(1u, prefix);
  key += std::to_string(index);
  return key;
}

static yyjson_val *assetObjectMember(yyjson_val *object,
                                     std::string_view key) {
  if (!yyjson_is_obj(object)) {
    return nullptr;
  }
  return yyjson_obj_getn(object, key.data(), key.size());
}

static double assetNumber(yyjson_val *parameters, std::string_view key,
                          double minimum, double maximum, double fallback) {
  auto *value = assetObjectMember(parameters, key);
  if (!yyjson_is_num(value)) {
    return fallback;
  }
  const auto number = yyjson_get_num(value);
  return std::isfinite(number) ? std::clamp(number, minimum, maximum)
                               : fallback;
}

static bool assetBoolean(yyjson_val *parameters, std::string_view key,
                         bool fallback) {
  auto *value = assetObjectMember(parameters, key);
  if (yyjson_is_bool(value)) {
    return yyjson_get_bool(value);
  }
  if (yyjson_is_num(value)) {
    return yyjson_get_num(value) != 0.0;
  }
  return fallback;
}

static std::string_view assetString(yyjson_val *parameters,
                                    std::string_view key,
                                    std::string_view fallback) {
  auto *value = assetObjectMember(parameters, key);
  return yyjson_is_str(value)
             ? std::string_view(yyjson_get_str(value), yyjson_get_len(value))
             : fallback;
}

static std::uint32_t assetHeadBlock(yyjson_val *parameters) {
  auto *value = assetObjectMember(parameters, "lt");
  auto candidate = std::int64_t{128};
  if (yyjson_is_str(value)) {
    const auto text =
        std::string_view(yyjson_get_str(value), yyjson_get_len(value));
    if (text == "0") {
      candidate = 0;
    } else if (text == "128") {
      candidate = 128;
    } else if (text == "256") {
      candidate = 256;
    } else if (text == "512") {
      candidate = 512;
    } else if (text == "1024") {
      candidate = 1024;
    }
  } else if (yyjson_is_int(value)) {
    candidate = yyjson_get_sint(value);
  }
  switch (candidate) {
  case 0:
  case 128:
  case 256:
  case 512:
  case 1024:
    return static_cast<std::uint32_t>(candidate);
  default:
    return 128u;
  }
}

static std::uint32_t assetTapCount(
    yyjson_val *parameters, std::span<const std::uint32_t> allowed,
    std::uint32_t fallback) {
  auto *value = assetObjectMember(parameters, "tp");
  if (!yyjson_is_num(value)) {
    return fallback;
  }
  const auto number = yyjson_get_num(value);
  if (!std::isfinite(number) || std::trunc(number) != number || number < 0.0 ||
      number > std::numeric_limits<std::uint32_t>::max()) {
    return fallback;
  }
  const auto candidate = static_cast<std::uint32_t>(number);
  return std::ranges::find(allowed, candidate) == allowed.end() ? fallback
                                                                : candidate;
}

static bool forwardGeneratedFirFft(GeneratedFirFftPlan &plan,
                                   std::span<const double> input,
                                   GeneratedFirSpectrum &spectrum,
                                   std::string &error) {
  if (plan.handle == nullptr || input.size() != plan.size) {
    error = "cannot prepare EffeTune's FIR design FFT";
    return false;
  }
  auto *fftInput = plan.api->designFftInput(plan.handle);
  if (fftInput == nullptr) {
    error = "EffeTune's FIR design FFT input is unavailable";
    return false;
  }
  for (auto index = std::size_t{0}; index < input.size(); ++index) {
    fftInput[index] = static_cast<float>(input[index]);
  }
  if (plan.api->designFftForward(plan.handle) != ET_OK) {
    error = "EffeTune's FIR design FFT forward transform failed";
    return false;
  }
  const auto *output = plan.api->designFftOutput(plan.handle);
  if (output == nullptr) {
    error = "EffeTune's FIR design FFT output is unavailable";
    return false;
  }
  const auto half = static_cast<std::size_t>(plan.size / 2u);
  spectrum.real.assign(half + 1u, 0.0);
  spectrum.imaginary.assign(half + 1u, 0.0);
  spectrum.real[0] = output[0];
  spectrum.real[half] = output[1];
  for (auto bin = std::size_t{1}; bin < half; ++bin) {
    spectrum.real[bin] = output[bin * 2u];
    spectrum.imaginary[bin] = output[bin * 2u + 1u];
  }
  return true;
}

static bool inverseGeneratedFirFft(GeneratedFirFftPlan &plan,
                                   std::span<const double> real,
                                   std::span<const double> imaginary,
                                   std::vector<double> &output,
                                   std::string &error) {
  const auto half = static_cast<std::size_t>(plan.size / 2u);
  if (plan.handle == nullptr || real.size() != half + 1u ||
      imaginary.size() != half + 1u) {
    error = "cannot prepare EffeTune's inverse FIR design FFT";
    return false;
  }
  auto *input = plan.api->designFftInput(plan.handle);
  if (input == nullptr) {
    error = "EffeTune's inverse FIR design FFT input is unavailable";
    return false;
  }
  std::fill_n(input, plan.size, 0.0F);
  input[0] = static_cast<float>(real[0]);
  input[1] = static_cast<float>(real[half]);
  for (auto bin = std::size_t{1}; bin < half; ++bin) {
    input[bin * 2u] = static_cast<float>(real[bin]);
    input[bin * 2u + 1u] = static_cast<float>(imaginary[bin]);
  }
  if (plan.api->designFftInverse(plan.handle) != ET_OK) {
    error = "EffeTune's FIR design FFT inverse transform failed";
    return false;
  }
  const auto *fftOutput = plan.api->designFftOutput(plan.handle);
  if (fftOutput == nullptr) {
    error = "EffeTune's inverse FIR design FFT output is unavailable";
    return false;
  }
  output.assign(fftOutput, fftOutput + plan.size);
  return true;
}

static std::uint64_t nextAssetPowerOfTwo(std::uint64_t value) {
  auto result = std::uint64_t{1u};
  while (result < value) {
    result *= 2u;
  }
  return result;
}

static std::uint64_t estimateGeneratedFirFootprint(
    std::uint32_t frames, std::uint32_t assetChannels,
    std::uint32_t topology, std::uint32_t processingChannels,
    std::uint32_t headBlock, std::uint32_t pathCount,
    std::uint32_t inputCount, std::uint64_t payloadBytes) {
  // Keep this upper bound aligned with EffeTune's ir-plugin-contract.js so the
  // native kernel can reserve every partition before the real-time path starts.
  const auto paths = topology == kMonoTopology ? processingChannels : pathCount;
  const auto inputs = topology == kMatrixTopology ? inputCount
                                                   : processingChannels;
  const auto latency = headBlock;
  const auto head = headBlock == 0u ? std::uint32_t{128u} : headBlock;
  auto requiredRing = std::uint64_t{latency} + 4096u;
  auto convolverBytes = std::uint64_t{512u};
  const auto addStage = [&](std::uint32_t block, std::uint32_t offset,
                            std::uint32_t end) {
    if (offset >= frames || end <= offset) {
      return;
    }
    const auto segmentFrames =
        static_cast<std::uint64_t>(std::min(end, frames) - offset);
    const auto required = static_cast<std::uint64_t>(latency) + offset +
                          block + 4096u;
    requiredRing = std::max(requiredRing, required);
    const auto fft = static_cast<std::uint64_t>(block) * 2u;
    const auto partitions = (segmentFrames + block - 1u) / block;
    const auto floatCount =
        3u * inputs * block + 2u * fft +
        (inputs + assetChannels) * partitions * fft +
        2u * processingChannels * fft;
    convolverBytes += 512u + floatCount * sizeof(float) +
                      nextAssetPowerOfTwo(paths) * 12u + 136u +
                      fft * sizeof(float);
  };
  addStage(head, latency == 0u ? 128u : 0u, 4u * head);
  for (auto block = 2u * head; block < 4096u; block *= 2u) {
    addStage(block, 2u * block, 4u * block);
  }
  addStage(4096u, 8192u, frames);
  convolverBytes += processingChannels * nextAssetPowerOfTwo(requiredRing) *
                    sizeof(float);
  if (latency == 0u) {
    convolverBytes +=
        (assetChannels + inputs) * 128u * sizeof(float);
  }
  convolverBytes += inputs * sizeof(float);
  const auto kernelBeginBound =
      payloadBytes + static_cast<std::uint64_t>(frames) * assetChannels * 16u +
      2u * 1024u * 1024u;
  return std::max(kernelBeginBound, payloadBytes + convolverBytes);
}

static void writeGeneratedFirUint32(std::span<std::uint8_t> bytes,
                                    std::size_t offset,
                                    std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xffu);
  bytes[offset + 1u] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
  bytes[offset + 2u] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
  bytes[offset + 3u] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

static bool buildGeneratedFirPayload(
    GeneratedFirAsset &result,
    std::span<const std::vector<float>> coefficientChannels,
    std::uint32_t sampleRate, std::uint32_t topology,
    std::span<const GeneratedFirPath> paths,
    std::uint32_t processingChannels, std::uint32_t inputCount,
    std::uint32_t headBlock) {
  if (coefficientChannels.empty() || coefficientChannels.front().empty()) {
    result.error = "generated FIR coefficients are empty";
    return false;
  }
  const auto frames = coefficientChannels.front().size();
  for (const auto &channel : coefficientChannels) {
    if (channel.size() != frames ||
        !std::ranges::all_of(channel,
                            [](float value) { return std::isfinite(value); })) {
      result.error = "generated FIR coefficients are invalid";
      return false;
    }
  }
  const auto payloadBytes =
      static_cast<std::uint64_t>(kAssetHeaderBytes) + paths.size() * 12u +
      static_cast<std::uint64_t>(coefficientChannels.size()) * frames *
          sizeof(float);
  const auto footprintBytes = estimateGeneratedFirFootprint(
      static_cast<std::uint32_t>(frames),
      static_cast<std::uint32_t>(coefficientChannels.size()), topology,
      processingChannels, headBlock, static_cast<std::uint32_t>(paths.size()),
      inputCount, payloadBytes);
  if (payloadBytes > kAssetCapacity || footprintBytes > kAssetCapacity ||
      payloadBytes > std::numeric_limits<std::uint32_t>::max() ||
      footprintBytes > std::numeric_limits<std::uint32_t>::max()) {
    result.error = "generated FIR asset exceeds EffeTune's 32 MiB limit";
    return false;
  }

  result.payload.assign(static_cast<std::size_t>(payloadBytes), 0u);
  auto payload = std::span<std::uint8_t>(result.payload);
  writeGeneratedFirUint32(payload, 0u, kAssetMagic);
  writeGeneratedFirUint32(
      payload, 4u, static_cast<std::uint32_t>(coefficientChannels.size()));
  writeGeneratedFirUint32(payload, 8u, static_cast<std::uint32_t>(frames));
  writeGeneratedFirUint32(payload, 12u, sampleRate);
  writeGeneratedFirUint32(payload, 16u, topology);
  writeGeneratedFirUint32(payload, 20u,
                          static_cast<std::uint32_t>(paths.size()));
  auto offset = std::size_t{kAssetHeaderBytes};
  for (const auto &path : paths) {
    writeGeneratedFirUint32(payload, offset, path.input);
    writeGeneratedFirUint32(payload, offset + 4u, path.output);
    writeGeneratedFirUint32(payload, offset + 8u, path.channel);
    offset += 12u;
  }
  for (const auto &channel : coefficientChannels) {
    for (const auto sample : channel) {
      writeGeneratedFirUint32(payload, offset, std::bit_cast<std::uint32_t>(sample));
      offset += sizeof(float);
    }
  }
  result.info = {
      .channels = static_cast<std::uint32_t>(coefficientChannels.size()),
      .frames = static_cast<std::uint32_t>(frames),
      .topology = topology,
      .head_block = headBlock,
      .rate_divider = 1u,
      .path_count = static_cast<std::uint32_t>(paths.size()),
      .input_count = inputCount,
      .processing_channels = processingChannels,
      .footprint_bytes = static_cast<std::uint32_t>(footprintBytes),
      .byte_size = static_cast<std::uint32_t>(payloadBytes),
  };
  return true;
}

static bool generatedFirMinimumPhase(
    GeneratedFirFftPlan &plan, std::span<const double> magnitudes,
    std::vector<double> &phase, std::string &error) {
  // The real cepstrum construction is the same minimum-phase transform used by
  // EffeTune's FIR Crossover and 5Band FIR PEQ designers.
  auto logMagnitude = std::vector<double>(magnitudes.size());
  for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
    logMagnitude[bin] = std::log(std::max(kMinimumMagnitude, magnitudes[bin]));
  }
  auto zeroImaginary = std::vector<double>(magnitudes.size(), 0.0);
  auto cepstrum = std::vector<double>();
  if (!inverseGeneratedFirFft(plan, logMagnitude, zeroImaginary, cepstrum,
                              error)) {
    return false;
  }
  for (auto index = std::size_t{1}; index < plan.size / 2u; ++index) {
    cepstrum[index] *= 2.0;
  }
  std::fill(cepstrum.begin() + plan.size / 2u + 1u, cepstrum.end(), 0.0);
  auto spectrum = GeneratedFirSpectrum{};
  if (!forwardGeneratedFirFft(plan, cepstrum, spectrum, error)) {
    return false;
  }
  phase = std::move(spectrum.imaginary);
  return true;
}

static std::vector<double> generatedFirWindow(std::uint32_t taps,
                                              bool minimumPhase) {
  auto window = std::vector<double>(taps, 1.0);
  if (minimumPhase) {
    const auto fadeStart = static_cast<std::uint32_t>(
        std::floor(static_cast<double>(taps) * 0.9));
    for (auto index = fadeStart; index < taps; ++index) {
      const auto denominator = std::max<std::uint32_t>(
          1u, taps - fadeStart - 1u);
      const auto fraction = static_cast<double>(index - fadeStart) /
                            static_cast<double>(denominator);
      window[index] = 0.5 +
                      0.5 * std::cos(std::numbers::pi * fraction);
    }
    return window;
  }
  const auto edge = static_cast<double>(taps) * 0.05;
  for (auto index = std::uint32_t{0}; index < taps; ++index) {
    if (static_cast<double>(index) < edge) {
      window[index] = 0.5 -
                      0.5 * std::cos(std::numbers::pi * index / edge);
    } else if (static_cast<double>(index) > taps - edge) {
      window[index] =
          0.5 - 0.5 * std::cos(std::numbers::pi * (taps - index) / edge);
    }
  }
  return window;
}

static double generatedFirCrossoverLowWeight(double frequency, double cutoff,
                                             double slope) {
  if (!(frequency > 0.0)) {
    return 1.0;
  }
  const auto octaveDecibels = 20.0 * std::log10(2.0);
  const auto exponent =
      slope / octaveDecibels * std::log(frequency / cutoff);
  if (exponent <= -36.0) {
    return 1.0;
  }
  if (exponent >= 36.0) {
    return 0.0;
  }
  return 1.0 / (1.0 + std::exp(exponent));
}

static GeneratedFirAsset designFirCrossover(
    yyjson_val *parameters, float requestedSampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api) {
  auto result = GeneratedFirAsset{};
  if (processingChannels < 4u || processingChannels > 16u ||
      processingChannels % 2u != 0u) {
    result.omissionReason = "requires an even channel output bus from 4 through 16";
    return result;
  }
  static constexpr std::array allowedTaps = {
      8192u, 16384u, 32768u, 65536u, 131072u};
  static constexpr std::array allowedSlopes = {
      24u, 48u, 72u, 96u, 144u, 192u, 288u, 384u};
  const auto sampleRate = static_cast<std::uint32_t>(std::clamp(
      std::round(static_cast<double>(requestedSampleRate)), 8000.0, 768000.0));
  const auto taps = assetTapCount(parameters, allowedTaps, 32768u);
  const auto phase = assetString(parameters, "pm", "min");
  const auto minimumPhase = phase != "lin";
  const auto requestedBands = static_cast<std::uint32_t>(std::clamp(
      std::round(assetNumber(parameters, "bc", 2.0, 4.0, 2.0)), 2.0, 4.0));
  const auto bandCount = std::min(requestedBands, processingChannels / 2u);
  const auto maximumFrequency = static_cast<double>(sampleRate) * 0.48;
  auto frequencies = std::array<double, 3>{2000.0, 4000.0, 8000.0};
  auto slopes = std::array<double, 3>{24.0, 24.0, 24.0};
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    const auto key = assetIndexedKey('f', index + 1u);
    frequencies[index] =
        assetNumber(parameters, key, 10.0, maximumFrequency,
                    std::clamp(frequencies[index], 10.0, maximumFrequency));
    const auto slopeKey = assetIndexedKey('s', index + 1u);
    const auto requestedSlope = static_cast<std::uint32_t>(std::abs(std::round(
        assetNumber(parameters, slopeKey, -384.0, 384.0, -24.0))));
    slopes[index] = std::ranges::find(allowedSlopes, requestedSlope) ==
                            allowedSlopes.end()
                        ? 24.0
                        : requestedSlope;
  }
  for (auto index = std::uint32_t{0}; index < bandCount - 1u; ++index) {
    const auto minimum = index == 0u ? 10.0 : frequencies[index - 1u] + 1.0;
    const auto maximum =
        maximumFrequency - static_cast<double>(bandCount - index - 2u);
    frequencies[index] = std::clamp(frequencies[index], minimum, maximum);
  }

  const auto fftSize = taps * 2u;
  auto plan = GeneratedFirFftPlan(api, fftSize);
  if (plan.handle == nullptr) {
    result.error = "cannot allocate EffeTune's FIR crossover design FFT";
    return result;
  }
  auto magnitudes = std::vector<std::vector<double>>(
      bandCount, std::vector<double>(fftSize / 2u + 1u, 0.0));
  for (auto bin = std::uint32_t{0}; bin <= fftSize / 2u; ++bin) {
    const auto frequency = static_cast<double>(bin) * sampleRate / fftSize;
    auto remainder = 1.0;
    for (auto crossover = std::uint32_t{0}; crossover < bandCount - 1u;
         ++crossover) {
      const auto low = generatedFirCrossoverLowWeight(
          frequency, frequencies[crossover], slopes[crossover]);
      magnitudes[crossover][bin] = remainder * low;
      remainder *= 1.0 - low;
    }
    magnitudes[bandCount - 1u][bin] = remainder;
  }

  const auto window = generatedFirWindow(taps, minimumPhase);
  auto channels = std::vector<std::vector<float>>();
  channels.reserve(bandCount);
  for (auto band = std::uint32_t{0}; band < bandCount; ++band) {
    auto real = std::vector<double>(fftSize / 2u + 1u, 0.0);
    auto imaginary = std::vector<double>(fftSize / 2u + 1u, 0.0);
    if (minimumPhase) {
      auto filterPhase = std::vector<double>();
      if (!generatedFirMinimumPhase(plan, magnitudes[band], filterPhase,
                                    result.error)) {
        return result;
      }
      for (auto bin = std::uint32_t{0}; bin <= fftSize / 2u; ++bin) {
        real[bin] = magnitudes[band][bin] * std::cos(filterPhase[bin]);
        imaginary[bin] =
            magnitudes[band][bin] * std::sin(filterPhase[bin]);
      }
    } else {
      for (auto bin = std::uint32_t{0}; bin <= fftSize / 2u; ++bin) {
        switch (bin & 3u) {
        case 0u:
          real[bin] = magnitudes[band][bin];
          break;
        case 1u:
          imaginary[bin] = -magnitudes[band][bin];
          break;
        case 2u:
          real[bin] = -magnitudes[band][bin];
          break;
        default:
          imaginary[bin] = magnitudes[band][bin];
          break;
        }
      }
    }
    imaginary.front() = 0.0;
    imaginary.back() = 0.0;
    auto time = std::vector<double>();
    if (!inverseGeneratedFirFft(plan, real, imaginary, time, result.error)) {
      return result;
    }
    auto channel = std::vector<float>(taps);
    for (auto index = std::uint32_t{0}; index < taps; ++index) {
      channel[index] = static_cast<float>(time[index] * window[index]);
    }
    channels.push_back(std::move(channel));
  }
  if (!minimumPhase) {
    auto &last = channels.back();
    const auto reconstructionDelay = taps / 2u;
    for (auto index = std::uint32_t{0}; index < taps; ++index) {
      auto value = index == reconstructionDelay ? 1.0F : 0.0F;
      for (auto band = std::uint32_t{0}; band < bandCount - 1u; ++band) {
        value -= channels[band][index];
      }
      last[index] = value;
    }
  }

  auto paths = std::vector<GeneratedFirPath>();
  paths.reserve(bandCount * 2u);
  for (auto band = std::uint32_t{0}; band < bandCount; ++band) {
    paths.push_back({.input = 0u, .output = band * 2u, .channel = band});
    paths.push_back(
        {.input = 1u, .output = band * 2u + 1u, .channel = band});
  }
  result.bandCount = bandCount;
  result.filterDelaySamples = minimumPhase ? 0u : taps / 2u;
  static_cast<void>(buildGeneratedFirPayload(
      result, channels, sampleRate, kMatrixTopology, paths,
      processingChannels, 2u, assetHeadBlock(parameters)));
  return result;
}

struct FiveBandFirCoefficients {
  double b0;
  double b1;
  double b2;
  double a1;
  double a2;
};

struct FiveBandFirBand {
  std::string_view type;
  double frequency;
  double gain;
  double q;
  double slope;
  bool enabled;
};

static FiveBandFirCoefficients fiveBandFirCoefficients(
    const FiveBandFirBand &band, double sampleRate) {
  const auto center = std::min(band.frequency, sampleRate * 0.49);
  const auto omega = 2.0 * std::numbers::pi * center / sampleRate;
  const auto cosine = std::cos(omega);
  const auto sine = std::sin(omega);
  const auto amplitude = std::pow(10.0, band.gain / 40.0);
  const auto alpha = sine / (2.0 * band.q);
  const auto root = std::sqrt(amplitude);
  auto b0 = 0.0;
  auto b1 = 0.0;
  auto b2 = 0.0;
  auto a0 = 0.0;
  auto a1 = 0.0;
  auto a2 = 0.0;
  if (band.type == "lp") {
    b0 = (1.0 - cosine) / 2.0;
    b1 = 1.0 - cosine;
    b2 = b0;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cosine;
    a2 = 1.0 - alpha;
  } else if (band.type == "hp") {
    b0 = (1.0 + cosine) / 2.0;
    b1 = -(1.0 + cosine);
    b2 = b0;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cosine;
    a2 = 1.0 - alpha;
  } else if (band.type == "ls") {
    b0 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine +
                      2.0 * root * alpha);
    b1 = 2.0 * amplitude *
         ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
    b2 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine -
                      2.0 * root * alpha);
    a0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine +
         2.0 * root * alpha;
    a1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
    a2 = (amplitude + 1.0) + (amplitude - 1.0) * cosine -
         2.0 * root * alpha;
  } else if (band.type == "hs") {
    b0 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine +
                      2.0 * root * alpha);
    b1 = -2.0 * amplitude *
         ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
    b2 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine -
                      2.0 * root * alpha);
    a0 = (amplitude + 1.0) - (amplitude - 1.0) * cosine +
         2.0 * root * alpha;
    a1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
    a2 = (amplitude + 1.0) - (amplitude - 1.0) * cosine -
         2.0 * root * alpha;
  } else if (band.type == "bp") {
    b0 = alpha;
    b1 = 0.0;
    b2 = -alpha;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cosine;
    a2 = 1.0 - alpha;
  } else if (band.type == "no") {
    b0 = 1.0;
    b1 = -2.0 * cosine;
    b2 = 1.0;
    a0 = 1.0 + alpha;
    a1 = -2.0 * cosine;
    a2 = 1.0 - alpha;
  } else {
    b0 = 1.0 + alpha * amplitude;
    b1 = -2.0 * cosine;
    b2 = 1.0 - alpha * amplitude;
    a0 = 1.0 + alpha / amplitude;
    a1 = -2.0 * cosine;
    a2 = 1.0 - alpha / amplitude;
  }
  const auto inverseA0 = 1.0 / a0;
  return {b0 * inverseA0, b1 * inverseA0, b2 * inverseA0,
          a1 * inverseA0, a2 * inverseA0};
}

static double fiveBandFirMagnitude(const FiveBandFirCoefficients &coefficients,
                                   double frequency, double sampleRate) {
  const auto omega = 2.0 * std::numbers::pi * frequency / sampleRate;
  const auto numeratorReal = coefficients.b0 +
                             coefficients.b1 * std::cos(omega) +
                             coefficients.b2 * std::cos(2.0 * omega);
  const auto numeratorImaginary =
      -coefficients.b1 * std::sin(omega) -
      coefficients.b2 * std::sin(2.0 * omega);
  const auto denominatorReal = 1.0 + coefficients.a1 * std::cos(omega) +
                               coefficients.a2 * std::cos(2.0 * omega);
  const auto denominatorImaginary =
      -coefficients.a1 * std::sin(omega) -
      coefficients.a2 * std::sin(2.0 * omega);
  return std::max(std::hypot(numeratorReal, numeratorImaginary),
                  kMinimumMagnitude) /
         std::max(std::hypot(denominatorReal, denominatorImaginary),
                  kMinimumMagnitude);
}

static GeneratedFirAsset designFiveBandFirPeq(
    yyjson_val *parameters, float requestedSampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api) {
  auto result = GeneratedFirAsset{};
  static constexpr std::array allowedTaps = {
      8192u, 16384u, 32768u, 65536u, 131072u};
  static constexpr std::array allowedTypes = {
      std::string_view{"pk"}, std::string_view{"lp"},
      std::string_view{"hp"}, std::string_view{"ls"},
      std::string_view{"hs"}, std::string_view{"bp"},
      std::string_view{"no"}};
  static constexpr std::array defaultFrequencies = {
      100.0, 316.0, 1000.0, 3160.0, 10000.0};
  const auto sampleRate = static_cast<std::uint32_t>(std::clamp(
      std::round(static_cast<double>(requestedSampleRate)), 8000.0, 768000.0));
  const auto taps = assetTapCount(parameters, allowedTaps, 32768u);
  const auto minimumPhase = assetString(parameters, "pm", "min") != "lin";
  const auto maximumFrequency =
      std::min(static_cast<double>(sampleRate) * 0.49, 20000.0);
  auto bands = std::array<FiveBandFirBand, 5>{};
  for (auto index = std::size_t{0}; index < bands.size(); ++index) {
    const auto suffix = std::to_string(index);
    const auto requestedType = assetString(parameters, "t" + suffix, "pk");
    const auto type = std::ranges::find(allowedTypes, requestedType) ==
                              allowedTypes.end()
                          ? std::string_view{"pk"}
                          : requestedType;
    bands[index] = {
        .type = type,
        .frequency = assetNumber(
            parameters, "f" + suffix, 20.0, maximumFrequency,
            std::min(defaultFrequencies[index], maximumFrequency)),
        .gain = assetNumber(parameters, "g" + suffix, -20.0, 20.0, 0.0),
        .q = assetNumber(parameters, "q" + suffix, 0.1, 100.0, 0.7),
        .slope =
            assetNumber(parameters, "s" + suffix, 0.1, 384.0, 12.0),
        .enabled = assetBoolean(parameters, "e" + suffix, true),
    };
  }

  const auto fftSize = taps * 2u;
  auto plan = GeneratedFirFftPlan(api, fftSize);
  if (plan.handle == nullptr) {
    result.error = "cannot allocate EffeTune's 5Band FIR PEQ design FFT";
    return result;
  }
  struct ActiveBand {
    FiveBandFirCoefficients coefficients;
    double exponent;
  };
  auto activeBands = std::vector<ActiveBand>();
  for (const auto &band : bands) {
    const auto alwaysActive = band.type == "lp" || band.type == "hp" ||
                              band.type == "bp" || band.type == "no";
    if (band.enabled && (alwaysActive || band.gain != 0.0)) {
      activeBands.push_back(
          {.coefficients = fiveBandFirCoefficients(band, sampleRate),
           .exponent = band.type == "lp" || band.type == "hp"
                           ? band.slope / 12.0
                           : 1.0});
    }
  }
  auto magnitudes = std::vector<double>(fftSize / 2u + 1u, 1.0);
  for (auto bin = std::uint32_t{0}; bin <= fftSize / 2u; ++bin) {
    const auto frequency = static_cast<double>(bin) * sampleRate / fftSize;
    auto magnitude = 1.0;
    for (const auto &band : activeBands) {
      const auto bandMagnitude =
          fiveBandFirMagnitude(band.coefficients, frequency, sampleRate);
      magnitude *= band.exponent != 1.0 && bandMagnitude < 1.0
                       ? std::pow(bandMagnitude, band.exponent)
                       : bandMagnitude;
    }
    magnitudes[bin] = std::max(magnitude, kMinimumMagnitude);
  }
  auto real = std::vector<double>(magnitudes.size(), 0.0);
  auto imaginary = std::vector<double>(magnitudes.size(), 0.0);
  if (minimumPhase) {
    auto phase = std::vector<double>();
    if (!generatedFirMinimumPhase(plan, magnitudes, phase, result.error)) {
      return result;
    }
    for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
      real[bin] = magnitudes[bin] * std::cos(phase[bin]);
      imaginary[bin] = magnitudes[bin] * std::sin(phase[bin]);
    }
  } else {
    for (auto bin = std::size_t{0}; bin < magnitudes.size(); ++bin) {
      switch (bin & 3u) {
      case 0u:
        real[bin] = magnitudes[bin];
        break;
      case 1u:
        imaginary[bin] = -magnitudes[bin];
        break;
      case 2u:
        real[bin] = -magnitudes[bin];
        break;
      default:
        imaginary[bin] = magnitudes[bin];
        break;
      }
    }
  }
  imaginary.front() = 0.0;
  imaginary.back() = 0.0;
  auto time = std::vector<double>();
  if (!inverseGeneratedFirFft(plan, real, imaginary, time, result.error)) {
    return result;
  }
  const auto window = generatedFirWindow(taps, minimumPhase);
  auto channels = std::vector<std::vector<float>>(1u,
                                                   std::vector<float>(taps));
  for (auto index = std::uint32_t{0}; index < taps; ++index) {
    channels[0][index] = static_cast<float>(time[index] * window[index]);
  }
  result.filterDelaySamples = minimumPhase ? 0u : taps / 2u;
  static_cast<void>(buildGeneratedFirPayload(
      result, channels, sampleRate, kMonoTopology,
      std::span<const GeneratedFirPath>{}, processingChannels, 0u,
      assetHeadBlock(parameters)));
  return result;
}

struct GroupDelayEqCurve {
  std::array<double, 15> positions{};
  std::array<double, 15> values{};
  std::array<double, 15> slopes{};
  double fadeStart = 0.0;
  double fadeEnd = 0.0;

  double evaluate(double frequency) const {
    static constexpr std::array frequencies = {
        25.0,   40.0,   63.0,   100.0,  160.0,
        250.0,  400.0,  630.0,  1000.0, 1600.0,
        2500.0, 4000.0, 6300.0, 10000.0, 16000.0};
    if (frequency >= fadeEnd) {
      return 0.0;
    }
    auto value = 0.0;
    if (frequency <= frequencies.front()) {
      value = values.front();
    } else if (frequency >= frequencies.back()) {
      value = values.back();
    } else {
      const auto position = std::log10(frequency);
      auto segment = std::size_t{0};
      while (segment < positions.size() - 2u &&
             position > positions[segment + 1u]) {
        ++segment;
      }
      const auto width = positions[segment + 1u] - positions[segment];
      const auto ratio = (position - positions[segment]) / width;
      const auto squared = ratio * ratio;
      const auto cubed = squared * ratio;
      value = (2.0 * cubed - 3.0 * squared + 1.0) * values[segment] +
              (cubed - 2.0 * squared + ratio) * width * slopes[segment] +
              (-2.0 * cubed + 3.0 * squared) * values[segment + 1u] +
              (cubed - squared) * width * slopes[segment + 1u];
    }
    if (frequency > fadeStart) {
      value *= 0.5 +
               0.5 * std::cos(std::numbers::pi *
                              (frequency - fadeStart) /
                              (fadeEnd - fadeStart));
    }
    return value;
  }
};

static GroupDelayEqCurve groupDelayEqCurve(
    const std::array<double, 15> &delays, double sampleRate) {
  static constexpr std::array frequencies = {
      25.0,   40.0,   63.0,   100.0,  160.0,
      250.0,  400.0,  630.0,  1000.0, 1600.0,
      2500.0, 4000.0, 6300.0, 10000.0, 16000.0};
  auto curve = GroupDelayEqCurve{};
  for (auto index = std::size_t{0}; index < frequencies.size(); ++index) {
    curve.positions[index] = std::log10(frequencies[index]);
    curve.values[index] = delays[index];
  }
  auto secants = std::array<double, 14>{};
  for (auto index = std::size_t{0}; index < secants.size(); ++index) {
    secants[index] =
        (curve.values[index + 1u] - curve.values[index]) /
        (curve.positions[index + 1u] - curve.positions[index]);
  }
  curve.slopes.front() = secants.front();
  curve.slopes.back() = secants.back();
  for (auto index = std::size_t{1}; index < curve.slopes.size() - 1u;
       ++index) {
    const auto previous = secants[index - 1u];
    const auto next = secants[index];
    if (previous * next <= 0.0) {
      continue;
    }
    const auto leftWidth =
        curve.positions[index] - curve.positions[index - 1u];
    const auto rightWidth =
        curve.positions[index + 1u] - curve.positions[index];
    const auto leftWeight = 2.0 * rightWidth + leftWidth;
    const auto rightWeight = rightWidth + 2.0 * leftWidth;
    curve.slopes[index] =
        (leftWeight + rightWeight) /
        (leftWeight / previous + rightWeight / next);
  }
  curve.fadeEnd = std::min(20000.0, sampleRate * 0.45);
  curve.fadeStart = std::min(frequencies.back(), curve.fadeEnd * 0.9);
  return curve;
}

struct GroupDelayPeqBand {
  std::string_view type;
  double frequency;
  double delay;
  double q;
  double logFrequency;
  double bellScale = 0.0;
  double shelfSlope = 0.0;
  double angularFrequency = 0.0;
  double filterNormalization = 0.0;
};

static double groupDelayPeqSecondOrder(double frequency,
                                       const GroupDelayPeqBand &band) {
  const auto omega = 2.0 * std::numbers::pi * frequency;
  const auto omegaSquared = omega * omega;
  const auto centerSquared =
      band.angularFrequency * band.angularFrequency;
  const auto difference = centerSquared - omegaSquared;
  const auto damping = band.angularFrequency * omega / band.q;
  return (band.angularFrequency / band.q) *
         (omegaSquared + centerSquared) /
         (difference * difference + damping * damping);
}

static double groupDelayPeqShape(double frequency,
                                 const GroupDelayPeqBand &band) {
  if (band.type == "pk") {
    const auto position =
        band.bellScale * (std::log2(frequency) - band.logFrequency);
    return band.delay *
           std::exp(-std::numbers::ln2 * position * position);
  }
  if (band.type == "ls") {
    const auto position = std::log2(frequency) - band.logFrequency;
    return band.delay / (1.0 + std::exp(band.shelfSlope * position));
  }
  if (band.type == "hs") {
    const auto position = band.logFrequency - std::log2(frequency);
    return band.delay / (1.0 + std::exp(band.shelfSlope * position));
  }
  return band.delay * groupDelayPeqSecondOrder(frequency, band) *
         band.filterNormalization;
}

struct GroupDelayPeqCurve {
  std::vector<GroupDelayPeqBand> bands;
  double limit = 0.0;
  double fadeStart = 0.0;
  double fadeEnd = 0.0;

  double evaluate(double frequency) const {
    if (frequency >= fadeEnd) {
      return 0.0;
    }
    const auto held = std::max(frequency, 20.0);
    auto value = 0.0;
    for (const auto &band : bands) {
      value += groupDelayPeqShape(held, band);
    }
    value = std::clamp(value, -limit, limit);
    if (frequency > fadeStart) {
      value *= 0.5 +
               0.5 * std::cos(std::numbers::pi *
                              (frequency - fadeStart) /
                              (fadeEnd - fadeStart));
    }
    return value;
  }
};

template <typename Curve>
static bool designGroupDelayImpulse(const Curve &curve, std::uint32_t taps,
                                    std::uint32_t sampleRate,
                                    const DspBackendApi &api,
                                    std::vector<float> &coefficients,
                                    std::string &error) {
  const auto fftSize = taps * 2u;
  auto plan = GeneratedFirFftPlan(api, fftSize);
  if (plan.handle == nullptr) {
    error = "cannot allocate EffeTune's group delay design FFT";
    return false;
  }
  const auto bins = static_cast<std::size_t>(fftSize / 2u + 1u);
  auto real = std::vector<double>(bins, 0.0);
  auto imaginary = std::vector<double>(bins, 0.0);
  const auto step = 2.0 * std::numbers::pi / fftSize;
  const auto samplesPerMillisecond = sampleRate / 1000.0;
  auto deviationPhase = 0.0;
  auto previous = curve.evaluate(0.0) * samplesPerMillisecond;
  real.front() = 1.0;
  for (auto bin = std::size_t{1}; bin < bins; ++bin) {
    const auto frequency =
        static_cast<double>(bin) * sampleRate / fftSize;
    const auto deviation =
        curve.evaluate(frequency) * samplesPerMillisecond;
    deviationPhase += 0.5 * (previous + deviation) * step;
    previous = deviation;
    const auto phase =
        -(step * static_cast<double>(bin) * (taps / 2u) + deviationPhase);
    real[bin] = std::cos(phase);
    imaginary[bin] = std::sin(phase);
  }
  real.back() = real.back() < 0.0 ? -1.0 : 1.0;
  imaginary.back() = 0.0;

  auto impulse = std::vector<double>();
  if (!inverseGeneratedFirFft(plan, real, imaginary, impulse, error)) {
    return false;
  }
  // Alternate the finite-length and unit-magnitude constraints exactly as the
  // EffeTune Group Delay designers do. This turns the ideal all-pass spectrum
  // into a bounded FIR without introducing a separate runtime dependency.
  for (auto pass = std::uint32_t{0}; pass < 12u; ++pass) {
    std::fill(impulse.begin() + taps, impulse.end(), 0.0);
    auto spectrum = GeneratedFirSpectrum{};
    if (!forwardGeneratedFirFft(plan, impulse, spectrum, error)) {
      return false;
    }
    auto converged = true;
    for (auto bin = std::size_t{0}; bin < spectrum.real.size(); ++bin) {
      const auto magnitude =
          std::hypot(spectrum.real[bin], spectrum.imaginary[bin]);
      if (magnitude < kMagnitudeEpsilon) {
        continue;
      }
      if (magnitude > 1.0001 || magnitude < 0.9999) {
        converged = false;
      }
      spectrum.real[bin] /= magnitude;
      spectrum.imaginary[bin] /= magnitude;
    }
    if (converged) {
      break;
    }
    if (!inverseGeneratedFirFft(plan, spectrum.real, spectrum.imaginary,
                                impulse, error)) {
      return false;
    }
  }
  coefficients.resize(taps);
  for (auto index = std::uint32_t{0}; index < taps; ++index) {
    coefficients[index] = static_cast<float>(impulse[index]);
  }
  return true;
}

static GeneratedFirAsset designGroupDelayEq(
    yyjson_val *parameters, float requestedSampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api) {
  auto result = GeneratedFirAsset{};
  static constexpr std::array allowedTaps = {4096u, 8192u, 16384u, 32768u};
  const auto sampleRate = static_cast<std::uint32_t>(std::clamp(
      std::round(static_cast<double>(requestedSampleRate)), 8000.0, 768000.0));
  const auto taps = assetTapCount(parameters, allowedTaps, 16384u);
  result.info.head_block = assetHeadBlock(parameters);
  const auto limit =
      (taps / 2.0 - taps / 16.0) * 1000.0 / sampleRate;
  auto delays = std::array<double, 15>{};
  auto hasDelay = false;
  for (auto index = std::size_t{0}; index < delays.size(); ++index) {
    delays[index] = assetNumber(parameters, assetIndexedKey('d', index),
                                -limit, limit, 0.0);
    hasDelay = hasDelay || delays[index] != 0.0;
  }
  result.filterDelaySamples = taps / 2u;
  if (!hasDelay) {
    return result;
  }
  const auto curve = groupDelayEqCurve(delays, sampleRate);
  auto channels = std::vector<std::vector<float>>(1u);
  if (!designGroupDelayImpulse(curve, taps, sampleRate, api, channels.front(),
                               result.error)) {
    return result;
  }
  static_cast<void>(buildGeneratedFirPayload(
      result, channels, sampleRate, kMonoTopology,
      std::span<const GeneratedFirPath>{}, processingChannels, 0u,
      assetHeadBlock(parameters)));
  return result;
}

static GeneratedFirAsset designGroupDelayPeq(
    yyjson_val *parameters, float requestedSampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api) {
  auto result = GeneratedFirAsset{};
  static constexpr std::array allowedTaps = {4096u, 8192u, 16384u, 32768u};
  static constexpr std::array allowedTypes = {
      std::string_view{"pk"}, std::string_view{"ls"},
      std::string_view{"hs"}, std::string_view{"fl"}};
  static constexpr std::array defaultFrequencies = {
      100.0, 316.0, 1000.0, 3160.0, 10000.0};
  const auto sampleRate = static_cast<std::uint32_t>(std::clamp(
      std::round(static_cast<double>(requestedSampleRate)), 8000.0, 768000.0));
  const auto taps = assetTapCount(parameters, allowedTaps, 16384u);
  result.info.head_block = assetHeadBlock(parameters);
  const auto rawLimit =
      (taps / 2.0 - taps / 16.0) * 1000.0 / sampleRate;
  const auto limit = std::floor(rawLimit * 10.0) / 10.0;
  auto curve = GroupDelayPeqCurve{
      .bands = {},
      .limit = limit,
      .fadeStart = std::min(20000.0, sampleRate * 0.45) * 0.9,
      .fadeEnd = std::min(20000.0, sampleRate * 0.45),
  };
  curve.bands.reserve(defaultFrequencies.size());
  for (auto index = std::size_t{0}; index < defaultFrequencies.size();
       ++index) {
    const auto suffix = std::to_string(index);
    if (!assetBoolean(parameters, "e" + suffix, true)) {
      continue;
    }
    const auto requestedType = assetString(parameters, "t" + suffix, "pk");
    const auto type = std::ranges::find(allowedTypes, requestedType) ==
                              allowedTypes.end()
                          ? std::string_view{"pk"}
                          : requestedType;
    const auto frequency = assetNumber(parameters, "f" + suffix, 20.0,
                                       20000.0,
                                       defaultFrequencies[index]);
    const auto delay =
        assetNumber(parameters, "d" + suffix, -limit, limit, 0.0);
    if (delay == 0.0) {
      continue;
    }
    const auto q =
        assetNumber(parameters, "q" + suffix, 0.1, 100.0, 0.7);
    auto band = GroupDelayPeqBand{
        .type = type,
        .frequency = frequency,
        .delay = delay,
        .q = q,
        .logFrequency = std::log2(frequency),
    };
    if (type == "pk") {
      const auto bandwidth =
          (2.0 / std::numbers::ln2) * std::asinh(1.0 / (2.0 * q));
      band.bellScale = 2.0 / bandwidth;
    } else if (type == "ls" || type == "hs") {
      band.shelfSlope = q * std::log(4.0);
    } else {
      band.angularFrequency = 2.0 * std::numbers::pi * frequency;
      auto extremumFrequency = 0.0;
      if (q > 1.0 / std::sqrt(3.0)) {
        const auto extremum = std::sqrt(4.0 - 1.0 / (q * q)) - 1.0;
        extremumFrequency = frequency * std::sqrt(extremum);
      }
      extremumFrequency =
          std::min(extremumFrequency, sampleRate / 2.0);
      band.filterNormalization =
          1.0 / groupDelayPeqSecondOrder(extremumFrequency, band);
    }
    curve.bands.push_back(band);
  }

  result.filterDelaySamples = taps / 2u;
  if (curve.bands.empty()) {
    return result;
  }
  auto channels = std::vector<std::vector<float>>(1u);
  if (!designGroupDelayImpulse(curve, taps, sampleRate, api, channels.front(),
                               result.error)) {
    return result;
  }
  static_cast<void>(buildGeneratedFirPayload(
      result, channels, sampleRate, kMonoTopology,
      std::span<const GeneratedFirPath>{}, processingChannels, 0u,
      assetHeadBlock(parameters)));
  return result;
}

bool supportsGeneratedFirAsset(std::string_view displayName) noexcept {
  return displayName == "FIR Crossover" || displayName == "5Band FIR PEQ" ||
         displayName == "Group Delay EQ" ||
         displayName == "Group Delay PEQ";
}

GeneratedFirAsset designGeneratedFirAsset(
    std::string_view displayName, yyjson_val *parameters, float sampleRate,
    std::uint32_t processingChannels, const DspBackendApi &api) {
  if (displayName == "FIR Crossover") {
    return designFirCrossover(parameters, sampleRate, processingChannels, api);
  }
  if (displayName == "5Band FIR PEQ") {
    return designFiveBandFirPeq(parameters, sampleRate, processingChannels,
                                api);
  }
  if (displayName == "Group Delay EQ") {
    return designGroupDelayEq(parameters, sampleRate, processingChannels, api);
  }
  if (displayName == "Group Delay PEQ") {
    return designGroupDelayPeq(parameters, sampleRate, processingChannels,
                               api);
  }
  auto result = GeneratedFirAsset{};
  result.error = "the DSP does not support generated FIR assets";
  return result;
}

} // namespace pipetune
