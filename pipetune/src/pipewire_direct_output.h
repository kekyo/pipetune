/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_PIPEWIRE_DIRECT_OUTPUT_H
#define PIPETUNE_PIPEWIRE_DIRECT_OUTPUT_H

#include <memory>
#include <string>

struct pw_core;
struct pw_stream;
struct spa_pod;

namespace pipetune {

/** Owns a direct, planar stream pair on the borrowed core's loop. */
struct PipeWireDirectOutput;
/** Stops capture before releasing playback and its borrowed buffers. */
struct PipeWireDirectOutputDeleter {
  /** Releases the pair on the core loop. */
  void operator()(PipeWireDirectOutput *output) const noexcept;
};
/** Exclusive ownership; must be released before disconnecting the core. */
using PipeWireDirectOutputPtr = std::unique_ptr<PipeWireDirectOutput, PipeWireDirectOutputDeleter>;
/** Handles input Props synchronously; use pw_stream_set_param to separate visible and effective gain. */
using PipeWireDirectControl = void (*)(pw_stream *input, const spa_pod *props, void *data);
/** Reports connection errors on the main loop; must not destroy the pair inline. */
using PipeWireDirectError = void (*)(const std::string &error, void *data);

/**
 * Creates a direct input with application channel positions mapped by index to AUX outputs.
 * @param core Borrowed PipeWire connection, outliving the result.
 * @param arguments Capture/playback property JSON from makeDirectOutputArguments.
 * @param control Required synchronous input control handler.
 * @param error Required connection failure handler.
 * @param data Borrowed callback context, outliving the result.
 * @return Connected pair, or null on allocation/configuration failure.
 * @remarks Audio is copied without gain, remixing, or an additional clock domain.
 */
PipeWireDirectOutputPtr createPipeWireDirectOutput(pw_core *core, const std::string &arguments,
    PipeWireDirectControl control, PipeWireDirectError error, void *data);

/** Returns the borrowed public input for publishing common controls. */
pw_stream *pipeWireDirectInput(PipeWireDirectOutput &output) noexcept;

} // namespace pipetune
#endif
