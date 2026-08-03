#ifndef PIPETUNE_PIPEWIRE_BUFFER_IO_H
#define PIPETUNE_PIPEWIRE_BUFFER_IO_H

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

} // namespace pipetune

#endif
