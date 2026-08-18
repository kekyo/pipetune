/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_TEST_PIPEWIRE_VERSION_H
#define PIPETUNE_TEST_PIPEWIRE_VERSION_H

#define PW_MAJOR 0
#define PW_MINOR 3
#define PW_MICRO 65

#define PW_CHECK_VERSION(major, minor, micro)                            \
  ((PW_MAJOR > (major)) ||                                               \
   (PW_MAJOR == (major) && PW_MINOR > (minor)) ||                        \
   (PW_MAJOR == (major) && PW_MINOR == (minor) &&                        \
    PW_MICRO >= (micro)))

#endif
