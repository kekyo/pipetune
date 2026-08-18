/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_stream_flags.h"

#include <cstdint>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static std::uint32_t flagBits(pw_stream_flags flags) noexcept {
  return static_cast<std::uint32_t>(flags);
}

static bool hasFlag(pw_stream_flags flags,
                    pw_stream_flags expected) noexcept {
  return (flagBits(flags) & flagBits(expected)) != 0;
}

static bool testInputUsesSupportedAsynchronousScheduling() {
  const auto flags = pipetune::makePipeWireStreamFlags(
      PW_DIRECTION_INPUT, true, true);
  if (!check(hasFlag(flags, PW_STREAM_FLAG_MAP_BUFFERS) &&
                 hasFlag(flags, PW_STREAM_FLAG_RT_PROCESS) &&
                 hasFlag(flags, PW_STREAM_FLAG_AUTOCONNECT),
             "input stream must retain its common connection flags") ||
      !check(!hasFlag(flags, PW_STREAM_FLAG_TRIGGER),
             "input stream must not become the triggered output")) {
    return false;
  }
#if PW_CHECK_VERSION(0, 3, 73)
  return check(hasFlag(flags, PW_STREAM_FLAG_ASYNC),
               "supported PipeWire must use asynchronous input");
#else
  return true;
#endif
}

static bool testOutputOwnsProcessingTrigger() {
  const auto flags = pipetune::makePipeWireStreamFlags(
      PW_DIRECTION_OUTPUT, false, false);
  return check(hasFlag(flags, PW_STREAM_FLAG_MAP_BUFFERS) &&
                   hasFlag(flags, PW_STREAM_FLAG_RT_PROCESS) &&
                   hasFlag(flags, PW_STREAM_FLAG_TRIGGER),
               "output stream must own the processing trigger") &&
         check(!hasFlag(flags, PW_STREAM_FLAG_AUTOCONNECT) &&
                   hasFlag(flags, PW_STREAM_FLAG_DONT_RECONNECT),
               "connection policy flags must remain independent");
}

int main() {
  return testInputUsesSupportedAsynchronousScheduling() &&
                 testOutputOwnsProcessingTrigger()
             ? 0
             : 1;
}
