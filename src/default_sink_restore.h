#ifndef PIPETUNE_DEFAULT_SINK_RESTORE_H
#define PIPETUNE_DEFAULT_SINK_RESTORE_H

#include <string>
#include <string_view>

namespace pipetune {

/**
 * Encodes a PipeWire default-node metadata JSON value.
 *
 * @param nodeName PipeWire node.name.
 * @return JSON object containing the name, or empty on allocation failure.
 */
std::string makeDefaultSinkMetadataValue(std::string_view nodeName);

/**
 * Extracts node.name from default-node metadata JSON.
 *
 * @param value NUL-terminated JSON, or null.
 * @return Decoded node name, or empty for missing or invalid input.
 */
std::string defaultSinkNameFromMetadata(const char *value);

/**
 * Reports completion of one fail-open default-sink restoration.
 */
struct DefaultSinkRestoreResult {
  /** True after the metadata write completed a PipeWire round-trip. */
  bool success;
  /** Selected physical node.name, or empty on failure. */
  std::string selectedTarget;
  /** Fatal connection, enumeration, or metadata diagnostic. */
  std::string error;
};

/**
 * Makes an available physical sink the effective PipeWire default.
 *
 * Virtual sinks and excludedNodeName are never selected. The current physical
 * default is retained when available; otherwise the highest session priority
 * wins.
 *
 * @param excludedNodeName PipeTune virtual sink name to exclude.
 * @return Completed restoration or a fatal diagnostic.
 */
DefaultSinkRestoreResult
restorePipeWireDefaultSink(std::string excludedNodeName);

} // namespace pipetune

#endif
