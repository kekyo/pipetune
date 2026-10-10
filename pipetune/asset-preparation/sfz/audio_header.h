/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_SFZ_AUDIO_HEADER_H
#define PIPETUNE_SFZ_AUDIO_HEADER_H
#include <cstddef>
#include <cstdint>
#include <span>

namespace pipetune::assets {

/** Dimensions used by the app's pre-decode capacity selection, all zero if unknown. */
struct SfzAudioHeader {
  std::uint32_t channels = 0; /**< Source channel count. */
  double sampleRate = 0; /**< Original rate obtained from the header. */
  std::uint64_t frames = 0; /**< Source frame estimate; actual decoded storage is checked later. */
};

/** Upper bound of the prefix covering every range requested by readSfzAudioHeader. */
inline constexpr std::size_t kSfzAudioHeaderBytes = 1024 * 1024 + 26;

/**
 * Inspects WAV/AIFF/FLAC headers using the application's scan and chunk-count limits.
 * @param prefix First bytes, bounded by kSfzAudioHeaderBytes; no decoded PCM is needed.
 * @param fileBytes Complete encoded file size for range validation.
 * @return Known positive dimensions, or zeros for malformed/unknown headers.
 * @remarks Compressed formats without those headers remain unknown even if a decoder
 * could estimate their duration. That distinction controls representative-layer selection.
 */
SfzAudioHeader inspectSfzAudioHeader(std::span<const std::uint8_t> prefix, std::uint64_t fileBytes);

} // namespace pipetune::assets
#endif
