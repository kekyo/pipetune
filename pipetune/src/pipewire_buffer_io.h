#ifndef PIPETUNE_PIPEWIRE_BUFFER_IO_H
#define PIPETUNE_PIPEWIRE_BUFFER_IO_H

#include <pipewire/stream.h>
#include <spa/buffer/buffer.h>
#include <spa/utils/defs.h>

#include <cstdint>

namespace pipetune {

/**
 * Converts PipeWire's graph time-domain fraction into a sample rate.
 *
 * @param rate Graph tick duration, normally expressed as 1/sample-rate.
 * @return Integral graph sample rate, or zero for an unusable fraction.
 */
std::uint32_t pipeWireGraphSampleRate(spa_fraction rate) noexcept;

/**
 * Reports whether a stream state boundary invalidates queued audio.
 *
 * The initial transition into PAUSED only completes format negotiation. A
 * stream that returns to PAUSED after STREAMING has stopped data transport,
 * so samples retained across that boundary belong to the previous activation.
 *
 * @param previousState State reported before the transition.
 * @param state State reported after the transition.
 * @return True when queued PCM must not survive the transition.
 */
bool pipeWireStateTransitionInvalidatesQueuedAudio(
    pw_stream_state previousState, pw_stream_state state) noexcept;

/**
 * Validates planar floating-point capture data and reports its frame count.
 *
 * @param buffer Capture buffer whose valid chunks are inspected.
 * @param channelCount Number of planar chunks required by the stream format.
 * @param frameCount Receives the common number of complete valid frames.
 * @return True when all required planes have usable floating-point chunks.
 */
bool inspectPipeWireCaptureBuffer(const spa_buffer &buffer,
                                  std::uint32_t channelCount,
                                  std::uint32_t &frameCount) noexcept;

/**
 * Marks all data chunks in a consumed capture buffer as having no valid data.
 *
 * Sample storage, chunk layout, flags, and metadata are preserved so the
 * producer can publish new content without reallocating the buffer.
 *
 * @param buffer Capture buffer being returned to PipeWire for reuse.
 */
void retirePipeWireCaptureBuffer(spa_buffer &buffer) noexcept;

} // namespace pipetune

#endif
