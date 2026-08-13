/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_EFFETUNE_BACKEND_ABI_H
#define PIPETUNE_EFFETUNE_BACKEND_ABI_H

#include <stdint.h>

#if defined(__GNUC__) || defined(__clang__)
#define PIPETUNE_EFFETUNE_BACKEND_EXPORT                                 \
  __attribute__((used, visibility("default")))
#else
#define PIPETUNE_EFFETUNE_BACKEND_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Identifies the concrete instruction-set variant of a PipeTune-owned
 * EffeTune backend artifact.
 */
enum {
  /** Scalar PFFFT and the normal compiler optimization policy. */
  PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR = 0u,
  /** Architecture SIMD PFFFT and baseline auto-vectorization. */
  PIPETUNE_EFFETUNE_BACKEND_VARIANT_SIMD_BASELINE = 1u,
  /** x86-64-v3 auto-vectorization with the x86 PFFFT implementation. */
  PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V3 = 2u,
  /** x86-64-v4 auto-vectorization with the x86 PFFFT implementation. */
  PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V4 = 3u,
  /** Arm SVE auto-vectorization with the AArch64 PFFFT implementation. */
  PIPETUNE_EFFETUNE_BACKEND_VARIANT_ARM64_SVE = 4u
};

/**
 * Returns the concrete instruction-set variant of this backend artifact.
 *
 * @return One of the `PIPETUNE_EFFETUNE_BACKEND_VARIANT_*` constants.
 */
PIPETUNE_EFFETUNE_BACKEND_EXPORT uint32_t
pipetune_effetune_backend_variant(void);

#ifdef __cplusplus
}
#endif

#endif
