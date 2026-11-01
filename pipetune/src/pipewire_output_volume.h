/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_OUTPUT_VOLUME_H
#define PIPETUNE_PIPEWIRE_OUTPUT_VOLUME_H

#include <cstdint>
#include <memory>
#include <string>

struct pw_core;

namespace pipetune {

/** Applies the common gain to every current generation of the mixed bus. */
struct PipeWireOutputVolume;
/** Releases volume bindings on the borrowed core's main loop. */
struct PipeWireOutputVolumeDeleter {
  /** Releases the controller before disconnecting its core. */
  void operator()(PipeWireOutputVolume *volume) const noexcept;
};
/** Exclusive ownership of the common output volume controller. */
using PipeWireOutputVolumePtr = std::unique_ptr<PipeWireOutputVolume, PipeWireOutputVolumeDeleter>;
/** Reports asynchronous write failures on the main loop; must not destroy the controller inline. */
using PipeWireVolumeError = void (*)(const std::string &error, void *data);

/**
 * Observes the named distribution bus, initially muted until controls are restored.
 * @param core Borrowed connection, outliving the controller.
 * @param name Exact private distribution node name.
 * @param error Required error handler.
 * @param data Borrowed callback context, outliving the controller.
 * @return Controller, or null if registry creation fails.
 */
PipeWireOutputVolumePtr createPipeWireOutputVolume(pw_core *core, const std::string &name,
    PipeWireVolumeError error, void *data);

/**
 * Applies a uniform linear gain after mixing, also retained for bus recreation.
 * @param volume Main-loop-owned controller.
 * @param channels Current bus width, from two to sixteen.
 * @param gain Finite nonnegative linear gain, independent of mute.
 * @param muted True while muted or awaiting restoration.
 * @return True when all current writes were accepted; asynchronous failures use the callback.
 */
bool setPipeWireOutputVolume(PipeWireOutputVolume &volume, std::uint32_t channels, float gain, bool muted);

} // namespace pipetune
#endif
