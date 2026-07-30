#include "pipewire_buffer_io.h"

#include <pipewire/keys.h>
#include <spa/buffer/meta.h>

#include <algorithm>
#include <cstddef>

namespace pipetune {

bool pipeWireBufferHasGap(const spa_buffer &buffer) noexcept {
  const auto *header = static_cast<const spa_meta_header *>(
      spa_buffer_find_meta_data(&buffer, SPA_META_Header,
                                sizeof(spa_meta_header)));
  return header != nullptr &&
         (header->flags & SPA_META_HEADER_FLAG_GAP) != 0;
}

void setPipeWirePlaybackContent(spa_buffer &buffer,
                                std::uint32_t channelCount,
                                std::uint32_t frameCount,
                                bool gap) noexcept {
  const auto availableChannels =
      buffer.datas == nullptr ? 0 : std::min(buffer.n_datas, channelCount);
  for (auto channel = std::uint32_t{0}; channel < availableChannels;
       ++channel) {
    auto *chunk = buffer.datas[channel].chunk;
    if (chunk == nullptr) {
      continue;
    }
    chunk->offset = 0;
    chunk->size =
        frameCount * static_cast<std::uint32_t>(sizeof(float));
    chunk->stride = static_cast<std::int32_t>(sizeof(float));
    if (gap) {
      chunk->flags |= SPA_CHUNK_FLAG_EMPTY;
    } else {
      chunk->flags &= ~SPA_CHUNK_FLAG_EMPTY;
    }
  }

  auto *header = static_cast<spa_meta_header *>(
      spa_buffer_find_meta_data(&buffer, SPA_META_Header,
                                sizeof(spa_meta_header)));
  if (header == nullptr) {
    return;
  }
  if (gap) {
    header->flags |= SPA_META_HEADER_FLAG_GAP;
  } else {
    header->flags &= ~SPA_META_HEADER_FLAG_GAP;
  }
}

void setPipeWireIdleProperties(pw_properties &properties) noexcept {
  static_cast<void>(
      pw_properties_set(&properties, PW_KEY_NODE_PAUSE_ON_IDLE, "true"));
  static_cast<void>(
      pw_properties_set(&properties, PW_KEY_NODE_SUSPEND_ON_IDLE, "false"));
}

} // namespace pipetune
