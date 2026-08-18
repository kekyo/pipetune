/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_STREAM_FLAGS_H
#define PIPETUNE_PIPEWIRE_STREAM_FLAGS_H

#include <pipewire/stream.h>
#include <pipewire/version.h>

#include <cstdint>

namespace pipetune {

/**
 * Builds the PipeWire flags used to connect one PipeTune audio stream.
 *
 * @param direction Input for the virtual sink or output for playback.
 * @param autoconnect True to let PipeWire select and connect a target.
 * @param reconnect True to reconnect after a target disappears.
 * @return Flags for pw_stream_connect().
 */
inline pw_stream_flags makePipeWireStreamFlags(
    pw_direction direction, bool autoconnect, bool reconnect) noexcept {
  auto flags = std::uint32_t{PW_STREAM_FLAG_MAP_BUFFERS} |
               std::uint32_t{PW_STREAM_FLAG_RT_PROCESS};
  if (direction == PW_DIRECTION_INPUT) {
#if PW_CHECK_VERSION(0, 3, 73)
    flags |= std::uint32_t{PW_STREAM_FLAG_ASYNC};
#endif
  } else {
    flags |= std::uint32_t{PW_STREAM_FLAG_TRIGGER};
  }
  if (autoconnect) {
    flags |= std::uint32_t{PW_STREAM_FLAG_AUTOCONNECT};
  }
  if (!reconnect) {
    flags |= std::uint32_t{PW_STREAM_FLAG_DONT_RECONNECT};
  }
  return static_cast<pw_stream_flags>(flags);
}

} // namespace pipetune

#endif
