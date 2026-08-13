/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_buffer_io.h"

#include <spa/buffer/meta.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

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

int main() {
  const auto passed = testConsumedCaptureContentIsRetired() &&
                      testGraphSampleRateUsesPipeWireTimeDomain() &&
                      testRuntimePauseInvalidatesQueuedAudio();
  return passed ? 0 : 1;
}
