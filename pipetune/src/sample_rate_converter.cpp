/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "sample_rate_converter.h"

#include <samplerate.h>

#include <algorithm>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace pipetune {

static std::size_t converterSampleCount(std::uint32_t channelCount,
                                        std::uint32_t maximumFrameCount) {
  if (channelCount == 0 || maximumFrameCount == 0 ||
      channelCount > static_cast<std::uint32_t>(INT_MAX) ||
      maximumFrameCount > static_cast<std::uint32_t>(LONG_MAX)) {
    throw std::invalid_argument(
        "sample-rate converter bounds must be positive");
  }
  return static_cast<std::size_t>(channelCount) * maximumFrameCount;
}

SampleRateBridgeRates resolveSampleRateBridgeRates(
    const SampleRatePolicy &policy, std::uint32_t streamSampleRate) noexcept {
  if (!sampleRatePolicyIsValid(policy) || streamSampleRate == 0) {
    return {};
  }
  const auto dspSampleRate =
      dspSampleRateForPolicy(policy, streamSampleRate);
  if (dspSampleRate == 0) {
    return {};
  }
  return {.streamSampleRate = streamSampleRate,
          .dspSampleRate = dspSampleRate,
          .conversionRequired = streamSampleRate != dspSampleRate};
}

PlanarSampleRateConverter::PlanarSampleRateConverter(
    std::uint32_t channelCount, std::uint32_t maximumFrameCount)
    : channelCount_(channelCount), maximumFrameCount_(maximumFrameCount),
      state_(nullptr),
      interleavedInput_(converterSampleCount(channelCount, maximumFrameCount),
                        0.0F),
      interleavedOutput_(converterSampleCount(channelCount, maximumFrameCount),
                         0.0F),
      configured_(false), passthrough_(false), ratio_(1.0) {
  auto error = 0;
  state_ = src_new(SRC_SINC_FASTEST, static_cast<int>(channelCount), &error);
  if (state_ == nullptr) {
    const auto *description = src_strerror(error);
    throw std::runtime_error(
        description == nullptr
            ? "cannot create sample-rate converter"
            : "cannot create sample-rate converter: " +
                  std::string(description));
  }
}

PlanarSampleRateConverter::~PlanarSampleRateConverter() {
  state_ = src_delete(static_cast<SRC_STATE *>(state_));
}

int PlanarSampleRateConverter::configure(
    std::uint32_t inputSampleRate,
    std::uint32_t outputSampleRate) noexcept {
  configured_ = false;
  if (inputSampleRate == 0 || outputSampleRate == 0) {
    return -1;
  }
  const auto ratio = static_cast<double>(outputSampleRate) /
                     static_cast<double>(inputSampleRate);
  if (src_is_valid_ratio(ratio) == 0) {
    return -1;
  }
  auto *state = static_cast<SRC_STATE *>(state_);
  auto error = src_reset(state);
  if (error == 0) {
    error = src_set_ratio(state, ratio);
  }
  if (error != 0) {
    return error;
  }
  ratio_ = ratio;
  passthrough_ = inputSampleRate == outputSampleRate;
  configured_ = true;
  return 0;
}

SampleRateConversionResult PlanarSampleRateConverter::process(
    std::span<const float> planarInput, std::uint32_t totalInputFrames,
    std::uint32_t inputFrameOffset, std::uint32_t inputFrameCount,
    std::span<float> planarOutput,
    std::uint32_t outputFrameCapacity) noexcept {
  const auto expectedInputSamples =
      static_cast<std::size_t>(channelCount_) * totalInputFrames;
  const auto expectedOutputSamples =
      static_cast<std::size_t>(channelCount_) * outputFrameCapacity;
  if (!configured_ || totalInputFrames > maximumFrameCount_ ||
      inputFrameOffset > totalInputFrames ||
      inputFrameCount > totalInputFrames - inputFrameOffset ||
      outputFrameCapacity > maximumFrameCount_ ||
      planarInput.size() != expectedInputSamples ||
      planarOutput.size() < expectedOutputSamples) {
    return {.inputFramesUsed = 0,
            .outputFramesGenerated = 0,
            .error = -1};
  }
  if (inputFrameCount == 0 || outputFrameCapacity == 0) {
    return {};
  }

  if (passthrough_) {
    const auto frameCount =
        std::min(inputFrameCount, outputFrameCapacity);
    for (auto channel = std::uint32_t{0}; channel < channelCount_;
         ++channel) {
      const auto source = planarInput.begin() +
                          static_cast<std::size_t>(channel) *
                              totalInputFrames +
                          inputFrameOffset;
      const auto destination =
          planarOutput.begin() +
          static_cast<std::size_t>(channel) * frameCount;
      std::copy_n(source, frameCount, destination);
    }
    return {.inputFramesUsed = frameCount,
            .outputFramesGenerated = frameCount,
            .error = 0};
  }

  for (auto frame = std::uint32_t{0}; frame < inputFrameCount; ++frame) {
    for (auto channel = std::uint32_t{0}; channel < channelCount_;
         ++channel) {
      interleavedInput_[static_cast<std::size_t>(frame) * channelCount_ +
                        channel] =
          planarInput[static_cast<std::size_t>(channel) *
                          totalInputFrames +
                      inputFrameOffset + frame];
    }
  }

  auto data = SRC_DATA{
      .data_in = interleavedInput_.data(),
      .data_out = interleavedOutput_.data(),
      .input_frames = static_cast<long>(inputFrameCount),
      .output_frames = static_cast<long>(outputFrameCapacity),
      .input_frames_used = 0,
      .output_frames_gen = 0,
      .end_of_input = 0,
      .src_ratio = ratio_,
  };
  const auto error = src_process(static_cast<SRC_STATE *>(state_), &data);
  if (error != 0 || data.input_frames_used < 0 ||
      data.output_frames_gen < 0) {
    return {.inputFramesUsed = 0,
            .outputFramesGenerated = 0,
            .error = error == 0 ? -1 : error};
  }
  const auto inputFramesUsed =
      static_cast<std::uint32_t>(data.input_frames_used);
  const auto outputFramesGenerated =
      static_cast<std::uint32_t>(data.output_frames_gen);
  for (auto channel = std::uint32_t{0}; channel < channelCount_;
       ++channel) {
    for (auto frame = std::uint32_t{0}; frame < outputFramesGenerated;
         ++frame) {
      planarOutput[static_cast<std::size_t>(channel) *
                       outputFramesGenerated +
                   frame] =
          interleavedOutput_[static_cast<std::size_t>(frame) * channelCount_ +
                             channel];
    }
  }
  return {.inputFramesUsed = inputFramesUsed,
          .outputFramesGenerated = outputFramesGenerated,
          .error = 0};
}

SampleRateBridgeDelayMeasurement measureSampleRateBridgeDelay(
    std::uint32_t streamSampleRate,
    std::uint32_t dspSampleRate) noexcept {
  if (streamSampleRate == 0 || dspSampleRate == 0) {
    return {.delayFrames = 0, .error = -1};
  }
  if (streamSampleRate == dspSampleRate) {
    return {.delayFrames = 0, .error = 0};
  }

  constexpr auto maximumFrames = std::uint32_t{8192};
  constexpr auto probeBlockFrames = std::uint32_t{32};
  constexpr auto totalProbeFrames = std::uint32_t{16384};
  try {
    auto inputConverter =
        PlanarSampleRateConverter(1, maximumFrames);
    auto outputConverter =
        PlanarSampleRateConverter(1, maximumFrames);
    auto error = inputConverter.configure(streamSampleRate, dspSampleRate);
    if (error == 0) {
      error = outputConverter.configure(dspSampleRate, streamSampleRate);
    }
    if (error != 0) {
      return {.delayFrames = 0, .error = error};
    }

    auto input = std::vector<float>(probeBlockFrames, 0.0F);
    auto dsp = std::vector<float>(maximumFrames, 0.0F);
    auto output = std::vector<float>(maximumFrames, 0.0F);
    auto consumedStreamFrames = std::uint64_t{0};
    auto generatedStreamFrames = std::uint64_t{0};
    auto maximumDeficit = std::uint64_t{0};
    while (consumedStreamFrames < totalProbeFrames) {
      const auto currentBlock = static_cast<std::uint32_t>(
          std::min<std::uint64_t>(
              probeBlockFrames,
              totalProbeFrames - consumedStreamFrames));
      auto inputOffset = std::uint32_t{0};
      while (inputOffset < currentBlock) {
        const auto converted = inputConverter.process(
            std::span<const float>(input).first(currentBlock), currentBlock,
            inputOffset, currentBlock - inputOffset, dsp, maximumFrames);
        if (converted.error != 0 ||
            (converted.inputFramesUsed == 0 &&
             converted.outputFramesGenerated == 0)) {
          return {.delayFrames = 0,
                  .error = converted.error == 0 ? -1 : converted.error};
        }
        inputOffset += converted.inputFramesUsed;

        auto dspOffset = std::uint32_t{0};
        while (dspOffset < converted.outputFramesGenerated) {
          const auto restored = outputConverter.process(
              std::span<const float>(dsp).first(
                  converted.outputFramesGenerated),
              converted.outputFramesGenerated, dspOffset,
              converted.outputFramesGenerated - dspOffset, output,
              maximumFrames);
          if (restored.error != 0 ||
              (restored.inputFramesUsed == 0 &&
               restored.outputFramesGenerated == 0)) {
            return {.delayFrames = 0,
                    .error = restored.error == 0 ? -1 : restored.error};
          }
          dspOffset += restored.inputFramesUsed;
          generatedStreamFrames += restored.outputFramesGenerated;
        }
      }
      consumedStreamFrames += currentBlock;
      if (consumedStreamFrames > generatedStreamFrames) {
        maximumDeficit = std::max(
            maximumDeficit,
            consumedStreamFrames - generatedStreamFrames);
      }
    }
    if (maximumDeficit >= UINT32_MAX) {
      return {.delayFrames = 0, .error = -1};
    }
    return {
        .delayFrames = static_cast<std::uint32_t>(maximumDeficit) + 1,
        .error = 0,
    };
  } catch (...) {
    return {.delayFrames = 0, .error = -1};
  }
}

} // namespace pipetune
