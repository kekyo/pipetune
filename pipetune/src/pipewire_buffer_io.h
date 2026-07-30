#ifndef PIPETUNE_PIPEWIRE_BUFFER_IO_H
#define PIPETUNE_PIPEWIRE_BUFFER_IO_H

#include <pipewire/properties.h>
#include <spa/buffer/buffer.h>

#include <cstdint>

namespace pipetune {

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

/**
 * Returns whether a PipeWire buffer header identifies neutral media.
 *
 * @param buffer Buffer whose optional header metadata is inspected.
 * @return True only when header metadata exists and carries the GAP flag.
 */
bool pipeWireBufferHasGap(const spa_buffer &buffer) noexcept;

/**
 * Publishes PCM or neutral-media state on a playback buffer.
 *
 * Existing chunk and header flags unrelated to EMPTY/GAP are preserved.
 *
 * @param buffer Playback buffer to update.
 * @param channelCount Number of planar chunks to update.
 * @param frameCount Number of frames represented by each chunk.
 * @param gap Whether the complete buffer contains neutral media.
 */
void setPipeWirePlaybackContent(spa_buffer &buffer,
                                std::uint32_t channelCount,
                                std::uint32_t frameCount,
                                bool gap) noexcept;

/**
 * Configures a node to pause, but not suspend, while idle.
 *
 * @param properties PipeWire node properties to update.
 */
void setPipeWireIdleProperties(pw_properties &properties) noexcept;

} // namespace pipetune

#endif
