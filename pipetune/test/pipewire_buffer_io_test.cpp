/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_buffer_io.h"
#include "pipetune/dsp_pipeline.h"

#include <spa/buffer/meta.h>

#include <array>
#include <algorithm>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <vector>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testConsumedCaptureContentIsRetired() {
  auto header = spa_meta_header{};
  header.flags = SPA_META_HEADER_FLAG_DISCONT;
  auto meta = spa_meta{
      .type = SPA_META_Header,
      .size = sizeof(header),
      .data = &header,
  };
  auto samples = std::array<std::array<float, 3>, 2>{
      std::array<float, 3>{1.0F, 2.0F, 3.0F},
      std::array<float, 3>{4.0F, 5.0F, 6.0F},
  };
  auto chunks = std::array<spa_chunk, 2>{};
  auto datas = std::array<spa_data, 2>{};
  for (auto channel = std::size_t{0}; channel < datas.size(); ++channel) {
    chunks[channel].offset = sizeof(float);
    chunks[channel].size = 2 * sizeof(float);
    chunks[channel].stride = static_cast<std::int32_t>(sizeof(float));
    chunks[channel].flags = SPA_CHUNK_FLAG_CORRUPTED;
    datas[channel].flags = SPA_DATA_FLAG_READABLE;
    datas[channel].maxsize = samples[channel].size() * sizeof(float);
    datas[channel].data = samples[channel].data();
    datas[channel].chunk = &chunks[channel];
  }
  auto buffer = spa_buffer{
      .n_metas = 1,
      .n_datas = 2,
      .metas = &meta,
      .datas = datas.data(),
  };

  auto frameCount = std::uint32_t{0};
  if (!check(pipetune::inspectPipeWireCaptureBuffer(buffer, 2, frameCount) &&
                 frameCount == 2,
             "capture chunks must initially expose their valid frames")) {
    return false;
  }

  pipetune::retirePipeWireCaptureBuffer(buffer);
  if (!check(pipetune::inspectPipeWireCaptureBuffer(buffer, 2, frameCount) &&
                 frameCount == 0,
             "consumed capture chunks must expose no valid frames") ||
      !check(samples[0] == std::array<float, 3>{1.0F, 2.0F, 3.0F} &&
                 samples[1] == std::array<float, 3>{4.0F, 5.0F, 6.0F},
             "retiring capture content must not rewrite sample storage") ||
      !check(chunks[0].offset == sizeof(float) &&
                 chunks[0].stride ==
                     static_cast<std::int32_t>(sizeof(float)) &&
                 chunks[0].flags == SPA_CHUNK_FLAG_CORRUPTED &&
                 header.flags == SPA_META_HEADER_FLAG_DISCONT,
             "retiring capture content must preserve buffer structure")) {
    return false;
  }

  samples[0][1] = 7.0F;
  samples[1][1] = 8.0F;
  chunks[0].size = sizeof(float);
  chunks[1].size = sizeof(float);
  return check(
      pipetune::inspectPipeWireCaptureBuffer(buffer, 2, frameCount) &&
          frameCount == 1,
      "new producer content must be visible from its first valid frame");
}

static bool testGraphSampleRateUsesPipeWireTimeDomain() {
  return check(pipetune::pipeWireGraphSampleRate({1, 48000}) == 48000,
               "PipeWire graph time must expose its sample rate") &&
         check(pipetune::pipeWireGraphSampleRate({2, 96000}) == 48000,
               "equivalent PipeWire graph fractions must be reduced") &&
         check(pipetune::pipeWireGraphSampleRate({0, 48000}) == 0 &&
                   pipetune::pipeWireGraphSampleRate({7, 48000}) == 0,
               "invalid graph time fractions must be unavailable");
}

static bool testRuntimePauseInvalidatesQueuedAudio() {
  return check(
             pipetune::pipeWireStateTransitionInvalidatesQueuedAudio(
                 PW_STREAM_STATE_STREAMING, PW_STREAM_STATE_PAUSED),
             "a runtime stream pause must invalidate queued PCM") &&
         check(
             !pipetune::pipeWireStateTransitionInvalidatesQueuedAudio(
                 PW_STREAM_STATE_CONNECTING, PW_STREAM_STATE_PAUSED),
             "initial format negotiation must not be a runtime pause") &&
         check(
             !pipetune::pipeWireStateTransitionInvalidatesQueuedAudio(
                 PW_STREAM_STATE_PAUSED, PW_STREAM_STATE_STREAMING),
             "stream resume must not discard newly queued PCM");
}

static bool testStereoIntoWiderDsp(const char *presetPath) {
  auto samples = std::array<std::array<float, 4>, 2>{
      std::array<float, 4>{1, 2, 3, 4}, std::array<float, 4>{11, 12, 13, 14}};
  auto chunks = std::array<spa_chunk, 2>{};
  auto planes = std::array<spa_data, 2>{};
  for (auto channel = 0U; channel < 2; ++channel) {
    chunks[channel].offset = 3 * sizeof(float);
    chunks[channel].size = 4 * sizeof(float);
    chunks[channel].stride = sizeof(float);
    planes[channel].data = samples[channel].data();
    planes[channel].maxsize = 4 * sizeof(float);
    planes[channel].chunk = &chunks[channel];
  }
  const auto buffer = spa_buffer{.n_metas = 0, .n_datas = 2, .metas = nullptr, .datas = planes.data()};
  const auto build = pipetune::PipelineBuildOptions{.sampleRate = 48000, .maxChannels = 16, .maxFrames = 32};
  auto bypass = pipetune::createBypassDspPipeline(build);
  if (!check(bypass.pipeline != nullptr, bypass.error)) return false;
  for (const auto width : {2U, 4U, 16U}) {
    auto destination = std::vector<float>(width * 3, 99);
    auto expected = std::vector<float>(width * 3, 0);
    std::copy_n(samples[0].begin(), 3, expected.begin());
    std::copy_n(samples[1].begin(), 3, expected.begin() + 3);
    if (!check(pipetune::copyPipeWireCaptureBlock(buffer, 2, 1, 3, width, destination),
               "stereo capture must fit a wider DSP block") ||
        !check(destination == expected, "only Ch 1/2 may contain input; additional channels must be reset to silence") ||
        !check(bypass.pipeline->process(destination, width, 3, 0) == pipetune::ProcessStatus::ok && destination == expected,
               "Bypass must preserve stereo without duplicating it to additional outputs")) return false;
  }
  auto matrix = pipetune::loadDspPipeline(presetPath, build);
  if (!check(matrix.pipeline != nullptr && matrix.warnings.empty(), matrix.error)) return false;
  auto destination = std::vector<float>(12, 99);
  if (!check(pipetune::copyPipeWireCaptureBlock(buffer, 2, 1, 3, 4, destination) &&
             matrix.pipeline->process(destination, 4, 3, 0) == pipetune::ProcessStatus::ok &&
             destination == std::vector<float>{1, 2, 3, 11, 12, 13, 1, 2, 3, -11, -12, -13},
             "the EffeTune Matrix preset must explicitly create Ch 3/4 from stereo input")) return false;
  if (!check(!pipetune::copyPipeWireCaptureBlock(buffer, 2, 2, 3, 4, destination) &&
             destination == std::vector<float>{1, 2, 3, 11, 12, 13, 1, 2, 3, -11, -12, -13},
             "an invalid capture range must leave the DSP buffer unchanged")) return false;
  chunks[0].flags = SPA_CHUNK_FLAG_EMPTY;
  return check(pipetune::copyPipeWireCaptureBlock(buffer, 2, 0, 3, 4, destination) &&
               destination == std::vector<float>{0, 0, 0, 14, 11, 12, 0, 0, 0, 0, 0, 0},
               "wrapped and empty capture planes must clear previous DSP output on every block");
}

int main(int argc, char **argv) {
  if (argc != 2) return 1;
  const auto passed = testConsumedCaptureContentIsRetired() &&
                      testGraphSampleRateUsesPipeWireTimeDomain() &&
                      testRuntimePauseInvalidatesQueuedAudio() &&
                      testStereoIntoWiderDsp(argv[1]);
  return passed ? 0 : 1;
}
