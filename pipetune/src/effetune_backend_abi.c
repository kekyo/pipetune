/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "effetune_backend_abi.h"

#if !defined(PIPETUNE_EFFETUNE_BACKEND_VARIANT)
#error "PIPETUNE_EFFETUNE_BACKEND_VARIANT must identify this backend"
#endif

uint32_t pipetune_effetune_backend_variant(void) {
  return PIPETUNE_EFFETUNE_BACKEND_VARIANT;
}
