/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_TEST_PIPEWIRE_STREAM_H
#define PIPETUNE_TEST_PIPEWIRE_STREAM_H

enum pw_direction {
  PW_DIRECTION_INPUT = 0,
  PW_DIRECTION_OUTPUT = 1,
};

enum pw_stream_flags {
  PW_STREAM_FLAG_NONE = 0,
  PW_STREAM_FLAG_AUTOCONNECT = (1 << 0),
  PW_STREAM_FLAG_MAP_BUFFERS = (1 << 2),
  PW_STREAM_FLAG_RT_PROCESS = (1 << 4),
  PW_STREAM_FLAG_DONT_RECONNECT = (1 << 7),
  PW_STREAM_FLAG_TRIGGER = (1 << 9),
};

#endif
