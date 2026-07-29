#ifndef PIPETUNE_PIPEWIRE_RATE_PARSER_H
#define PIPETUNE_PIPEWIRE_RATE_PARSER_H

#include "pipetune/sample_rate.h"

#include <spa/param/param.h>
#include <spa/pod/pod.h>

#include <cstdint>
#include <vector>

namespace pipetune {

/**
 * Describes which node sample-rate parameters can currently be read.
 */
struct PipeWireRateParameterAvailability {
  bool enumFormatReadable;
  bool formatReadable;

  bool operator==(const PipeWireRateParameterAvailability &) const = default;
};

/**
 * Inspects a PipeWire node parameter list for readable rate information.
 *
 * @param parameters Parameter metadata reported by the node.
 * @param parameterCount Number of entries in parameters.
 * @return Readability of EnumFormat capabilities and the active Format.
 */
PipeWireRateParameterAvailability pipeWireRateParameterAvailability(
    const spa_param_info *parameters, std::uint32_t parameterCount);

/**
 * Accumulates one EnumFormat event into immediately usable capabilities.
 *
 * PipeWire reports the next enumeration index rather than a separate
 * completion event, so callers should publish the returned snapshot after
 * every parameter event.
 *
 * @param format PipeWire SPA_PARAM_EnumFormat value.
 * @param index Enumeration index for this value.
 * @param constraints Accumulator shared by events from one node.
 * @return Normalized known capabilities accumulated through this event.
 */
SampleRateCapabilities accumulatePipeWireSampleRateCapabilities(
    const spa_pod *format, std::uint32_t index,
    std::vector<SampleRateConstraint> &constraints);

/**
 * Appends sample-rate constraints from one PipeWire raw-audio EnumFormat pod.
 *
 * Non-raw or malformed formats are ignored without modifying constraints.
 *
 * @param format PipeWire SPA_PARAM_EnumFormat value.
 * @param constraints Destination for parsed constraints.
 * @return True when one or more valid constraints were appended.
 */
bool appendPipeWireSampleRateConstraints(
    const spa_pod *format,
    std::vector<SampleRateConstraint> &constraints);

} // namespace pipetune

#endif
