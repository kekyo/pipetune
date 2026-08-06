#include "pipewire_buffer_io.h"

#include <algorithm>
#include <cstddef>

namespace pipetune {

std::uint32_t pipeWireGraphSampleRate(spa_fraction rate) noexcept {
  if (rate.num == 0 || rate.denom == 0 || rate.denom % rate.num != 0) {
    return 0;
  }
  return rate.denom / rate.num;
}

bool pipeWireStateTransitionInvalidatesQueuedAudio(
    pw_stream_state previousState, pw_stream_state state) noexcept {
  return previousState == PW_STREAM_STATE_STREAMING &&
         state == PW_STREAM_STATE_PAUSED;
}

bool inspectPipeWireCaptureBuffer(const spa_buffer &buffer,
                                  std::uint32_t channelCount,
                                  std::uint32_t &frameCount) noexcept {
  if (buffer.n_datas < channelCount || buffer.datas == nullptr) {
    return false;
  }
  frameCount = UINT32_MAX;
  for (auto channel = std::uint32_t{0}; channel < channelCount; ++channel) {
    const auto &plane = buffer.datas[channel];
    const auto sampleBytes = std::uint32_t{sizeof(float)};
    if (plane.data == nullptr || plane.chunk == nullptr ||
        plane.maxsize < sampleBytes || plane.maxsize % sampleBytes != 0 ||
        plane.chunk->stride != static_cast<std::int32_t>(sampleBytes) ||
        plane.chunk->offset % sampleBytes != 0) {
      return false;
    }
    const auto byteCount = std::min(plane.chunk->size, plane.maxsize);
    frameCount = std::min(frameCount, byteCount / sampleBytes);
  }
  return frameCount != UINT32_MAX;
}

void retirePipeWireCaptureBuffer(spa_buffer &buffer) noexcept {
  if (buffer.datas == nullptr) {
    return;
  }
  for (auto index = std::uint32_t{0}; index < buffer.n_datas; ++index) {
    if (buffer.datas[index].chunk != nullptr) {
      buffer.datas[index].chunk->size = 0;
    }
  }
}

} // namespace pipetune
