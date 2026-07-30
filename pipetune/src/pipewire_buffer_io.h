#ifndef PIPETUNE_PIPEWIRE_BUFFER_IO_H
#define PIPETUNE_PIPEWIRE_BUFFER_IO_H

#include <pipewire/properties.h>
#include <spa/buffer/buffer.h>

#include <cstdint>

namespace pipetune {

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
