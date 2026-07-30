#include "pipewire_buffer_io.h"

#include <pipewire/keys.h>
#include <spa/buffer/meta.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool testHeaderGapIsDetected() {
  auto header = spa_meta_header{};
  auto meta = spa_meta{
      .type = SPA_META_Header,
      .size = sizeof(header),
      .data = &header,
  };
  auto buffer = spa_buffer{
      .n_metas = 1,
      .n_datas = 0,
      .metas = &meta,
      .datas = nullptr,
  };
  if (!check(!pipetune::pipeWireBufferHasGap(buffer),
             "clear header must not be reported as a gap")) {
    return false;
  }
  header.flags |= SPA_META_HEADER_FLAG_GAP;
  return check(pipetune::pipeWireBufferHasGap(buffer),
               "header GAP flag must be detected");
}

static bool testPlaybackFlagsPreserveUnrelatedState() {
  auto header = spa_meta_header{};
  header.flags = SPA_META_HEADER_FLAG_DISCONT;
  auto meta = spa_meta{
      .type = SPA_META_Header,
      .size = sizeof(header),
      .data = &header,
  };
  auto chunks = std::array<spa_chunk, 2>{};
  chunks[0].flags = SPA_CHUNK_FLAG_CORRUPTED;
  chunks[1].flags = SPA_CHUNK_FLAG_CORRUPTED;
  auto datas = std::array<spa_data, 2>{};
  datas[0].chunk = &chunks[0];
  datas[1].chunk = &chunks[1];
  auto buffer = spa_buffer{
      .n_metas = 1,
      .n_datas = 2,
      .metas = &meta,
      .datas = datas.data(),
  };

  pipetune::setPipeWirePlaybackContent(buffer, 2, 16, true);
  if (!check(
          chunks[0].offset == 0 && chunks[0].size == 16 * sizeof(float) &&
              chunks[0].stride == static_cast<std::int32_t>(sizeof(float)) &&
              (chunks[0].flags & SPA_CHUNK_FLAG_EMPTY) != 0 &&
              (chunks[0].flags & SPA_CHUNK_FLAG_CORRUPTED) != 0,
          "gap chunks must be complete, empty, and preserve other flags") ||
      !check((header.flags & SPA_META_HEADER_FLAG_GAP) != 0 &&
                 (header.flags & SPA_META_HEADER_FLAG_DISCONT) != 0,
             "gap header must preserve unrelated flags")) {
    return false;
  }

  pipetune::setPipeWirePlaybackContent(buffer, 2, 8, false);
  return check(chunks[0].size == 8 * sizeof(float) &&
                   (chunks[0].flags & SPA_CHUNK_FLAG_EMPTY) == 0 &&
                   (chunks[0].flags & SPA_CHUNK_FLAG_CORRUPTED) != 0,
               "PCM chunks must clear only EMPTY") &&
         check((header.flags & SPA_META_HEADER_FLAG_GAP) == 0 &&
                   (header.flags & SPA_META_HEADER_FLAG_DISCONT) != 0,
               "PCM header must clear only GAP");
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

static bool testIdlePropertiesPauseWithoutSuspending() {
  auto *properties = pw_properties_new(nullptr, nullptr);
  if (!check(properties != nullptr,
             "PipeWire properties must be allocated")) {
    return false;
  }
  pipetune::setPipeWireIdleProperties(*properties);
  const auto *pause =
      pw_properties_get(properties, PW_KEY_NODE_PAUSE_ON_IDLE);
  const auto *suspend =
      pw_properties_get(properties, PW_KEY_NODE_SUSPEND_ON_IDLE);
  const auto passed =
      check(pause != nullptr && std::strcmp(pause, "true") == 0,
            "idle nodes must pause") &&
      check(suspend != nullptr && std::strcmp(suspend, "false") == 0,
            "idle nodes must remain unsuspended");
  pw_properties_free(properties);
  return passed;
}

int main() {
  const auto passed =
      testHeaderGapIsDetected() &&
      testPlaybackFlagsPreserveUnrelatedState() &&
      testConsumedCaptureContentIsRetired() &&
      testIdlePropertiesPauseWithoutSuspending();
  return passed ? 0 : 1;
}
