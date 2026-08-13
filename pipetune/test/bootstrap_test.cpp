/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pffft.h>

#include <iostream>

int main() {
  if (pffft_simd_size() != 1) {
    std::cerr << "PipeTune must match EffeTune's scalar PFFFT configuration\n";
    return 1;
  }
  return 0;
}
