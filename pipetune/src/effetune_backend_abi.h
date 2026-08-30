/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_EFFETUNE_BACKEND_ABI_H
#define PIPETUNE_EFFETUNE_BACKEND_ABI_H

#include <effetune/abi.h>

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

/**
 * Describes one native EffeTune instance asset transfer.
 */
typedef struct pipetune_effetune_asset_info_v1 {
  /** Number of planar coefficient channels in the payload. */
  uint32_t channels;
  /** Number of coefficient frames per channel. */
  uint32_t frames;
  /** EffeTune impulse-response topology value. */
  uint32_t topology;
  /** Requested convolution head-block size. */
  uint32_t head_block;
  /** Convolution sample-rate divider. */
  uint32_t rate_divider;
  /** Number of matrix path records in the payload. */
  uint32_t path_count;
  /** Number of distinct matrix inputs. */
  uint32_t input_count;
  /** Number of audio channels processed by the instance. */
  uint32_t processing_channels;
  /** Conservative native allocation footprint in bytes. */
  uint32_t footprint_bytes;
  /** Exact payload byte count. */
  uint32_t byte_size;
} pipetune_effetune_asset_info_v1;

/**
 * Copies and commits one complete asset without exposing a native pointer.
 *
 * @param engine Native EffeTune engine handle.
 * @param instance Native EffeTune instance handle.
 * @param slot Asset slot to replace.
 * @param info Validated transfer metadata.
 * @param payload Complete asset payload owned by the caller.
 * @param payload_bytes Exact readable payload size.
 * @param format_tag EffeTune asset format identifier.
 * @return `ET_OK` on success, otherwise an EffeTune status code.
 */
PIPETUNE_EFFETUNE_BACKEND_EXPORT et_status
pipetune_effetune_instance_asset_copy_v1(
    et_engine engine, et_instance instance, uint32_t slot,
    const pipetune_effetune_asset_info_v1 *info, const uint8_t *payload,
    uint64_t payload_bytes, uint32_t format_tag);

#ifdef __cplusplus
}
#endif

#endif
