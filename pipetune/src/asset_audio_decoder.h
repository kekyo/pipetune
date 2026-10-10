/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_ASSET_AUDIO_DECODER_H
#define PIPETUNE_ASSET_AUDIO_DECODER_H

#include "common/audio.h"
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace pipetune {

/**
 * Decodes an in-memory audio file without opening additional files or URLs.
 * @param bytes Complete original file, at most 1 GiB; each asset loader enforces its own smaller limit.
 * @param maximumPcmBytes Maximum decoded float32 storage, checked incrementally.
 * @return Finite planar PCM at the original rate and channel order.
 * @throws std::runtime_error Invalid, unsupported, or oversized audio.
 */
assets::Audio decodeAssetAudio(std::span<const std::uint8_t> bytes,
                              std::size_t maximumPcmBytes);

/**
 * Converts complete IR channels with a bounded native band-limited resampler.
 * @param audio Owned source PCM; unchanged when the rates already match.
 * @param sampleRate Required convolution rate in hertz.
 * @param maximumPcmBytes Maximum output storage, checked before allocation.
 * @return PCM with rounded duration and unchanged channel order.
 * @throws std::runtime_error Invalid conversion, non-finite PCM, or exceeded budget.
 * @remarks Replaces the app's browser-provided OfflineAudioContext, not its JS measurement resampler.
 */
assets::Audio resampleIrAudio(assets::Audio audio, std::uint32_t sampleRate,
                            std::size_t maximumPcmBytes);

/** Returns the native decoder version used in preparation cache keys. */
std::string assetDecoderVersion();

} // namespace pipetune
#endif
