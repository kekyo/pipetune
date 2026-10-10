/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */

/*
 * SFZ semantics derived from EffeTune v2.13.0, Copyright (c) 2025-2026 Yoshiyuki Kobayashi.
 * See LICENSE.effetune and the preparation port map.
 */
#ifndef PIPETUNE_SFZ_REGION_H
#define PIPETUNE_SFZ_REGION_H
#include "common/audio.h"
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace pipetune::assets {

/** Preparation failure with the corresponding application diagnostic code. */
struct SfzError final : std::runtime_error {
  std::string code; /**< Stable error category from the app preparation contract. */
  std::string requiredPath; /**< Missing document to supply before retrying, or empty. */
  std::uint64_t minimumWorkingBytes = 0; /**< Minimum next parser allowance after a working-memory rejection. */
  /** Creates an error with a category and human-readable message. */
  SfzError(std::string category, std::string message, std::string missingDocument = {})
      : std::runtime_error(std::move(message)), code(std::move(category)), requiredPath(std::move(missingDocument)) {}
};

/** Non-fatal reduction or exclusion, preserving the application's warning categories. */
struct SfzWarning {
  std::string code; /**< Warning category. */
  std::uint32_t count; /**< Number of affected items. */
};

/** Normalized region. Opcode names intentionally match the upstream JS representation. */
struct SfzRegion {
  std::string sample; /**< Root-relative sample path. */
  std::uint32_t seqGroup = 0; /**< Inherited global/master/group sequence identity. */
  double lokey = 0; /**< Inclusive lowest MIDI key. */
  double hikey = 127; /**< Inclusive highest MIDI key. */
  double lovel = 1; /**< Inclusive lowest velocity. */
  double hivel = 127; /**< Inclusive highest velocity. */
  double lorand = 0; /**< Lower random selection bound. */
  double hirand = 1; /**< Upper random selection bound. */
  double seq_length = 1; /**< Round-robin cycle length. */
  double seq_position = 1; /**< One-based position in the cycle. */
  double pitch_keycenter = 60; /**< Original sample's MIDI pitch. */
  double pitch_keytrack = 100; /**< Pitch tracking in cents per key. */
  double transpose = 0; /**< Semitone transposition. */
  double tune = 0; /**< Fine transposition in cents. */
  double volume = 0; /**< Region gain in dB. */
  double pan = 0; /**< Pan from -100 through 100. */
  double amp_veltrack = 100; /**< Velocity tracking percentage. */
  double offset = 0; /**< First source frame. */
  std::optional<double> end; /**< Inclusive final frame, absent for the full sample. */
  double loop_mode = 0; /**< No loop, one shot, continuous, or sustain (0 through 3). */
  double loop_start = 0; /**< Inclusive loop start. */
  std::optional<double> loop_end; /**< Inclusive loop end, absent for the playback end. */
  double ampeg_attack = 0; /**< Attack duration in seconds. */
  double ampeg_hold = 0; /**< Hold duration in seconds. */
  double ampeg_decay = 0; /**< Decay duration in seconds. */
  double ampeg_sustain = 100; /**< Sustain percentage. */
  double ampeg_release = 0.001; /**< Release duration in seconds. */
};

/** Decoded source PCM, with no decoder or host ownership exposed. */
struct SfzSample {
  std::string path; /**< Root-relative normalized sample path. */
  Audio audio; /**< Original sample rate and mono/stereo planar float32 channels. */
};

/** Maximum supported bank, including its native index. */
inline constexpr std::uint64_t kMaximumSfzBytes = 1024ull * 1024 * 1024;

} // namespace pipetune::assets
#endif
