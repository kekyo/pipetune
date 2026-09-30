/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PRESET_ENTRY_H
#define PIPETUNE_PRESET_ENTRY_H

#include <string>

namespace pipetune {

/** Describes how a loaded preset entry participates in DSP processing. */
enum class PresetEntryState {
  /** Enabled DSP or section in the preset, independently of global bypass. */
  enabled,
  /** Disabled DSP or section, including DSPs gated by a disabled section. */
  off,
  /** Entry omitted by PipeTune, including visualizers and unavailable DSPs. */
  ignored
};

/** One entry in loaded preset order, including disabled and omitted entries. */
struct PresetEntry {
  /** Original display name from the preset. */
  std::string name;
  /** Effective entry state determined while loading the preset. */
  PresetEntryState state;

  /** Compares the displayed name and configuration state. */
  bool operator==(const PresetEntry &other) const = default;
};

} // namespace pipetune

#endif
