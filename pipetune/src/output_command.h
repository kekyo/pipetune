/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_OUTPUT_COMMAND_H
#define PIPETUNE_OUTPUT_COMMAND_H

#include "pipetune/control_protocol.h"

namespace pipetune {

/** Result of querying live routing and device presence. */
struct OutputStatusQueryResult {
  /** Confirmed status, meaningful only when error is empty. */
  ControlRuntimeStatus status = {};
  /** Original successful response for JSON clients. */
  std::string json;
  /** Transport or protocol diagnostic; empty on success. */
  std::string error;
  /** True only when the control endpoint has no listening daemon. */
  bool unavailable = false;
};

/** User operation to resolve against the latest complete configuration. */
enum class OutputChangeKind {
  /** Replace every routing choice with an explicit configuration. */
  replace,
  /** Change only the mode, retaining selected devices and fixed slots. */
  mode,
  /** Enable the named devices and reserve the slots of other devices. */
  select
};

/** Complete user intent for a persistent routing change. */
struct OutputChange {
  /** Operation to perform. */
  OutputChangeKind kind;
  /** Complete replacement, or only its mode for a mode operation. */
  OutputConfiguration configuration = {};
  /** Current node names for a selection operation, in allocation order. */
  std::vector<std::string> nodes = {};
};

/** Control and storage endpoints for a routing change. */
struct PersistentOutputOptions {
  /** Canonical startup configuration file. */
  std::filesystem::path configPath;
  /** Same-user daemon control socket. */
  std::filesystem::path socketPath;
};

/** Outcome of live application, persistence, and any necessary restoration. */
struct PersistentOutputResult {
  /** True only after persistence and any live application are confirmed. */
  bool success = false;
  /** True when the desired live configuration remains confirmed. */
  bool liveApplied = false;
  /** True when the new startup configuration was saved. */
  bool persistenceApplied = false;
  /** True after a save failure and confirmed restoration of previous routing. */
  bool rollbackApplied = false;
  /** Last confirmed daemon status, or defaults for an offline change. */
  ControlRuntimeStatus status = {};
  /** Explanation when a change is deferred to the next daemon start. */
  std::string notice;
  /** Failure diagnostic, including uncertainty or failed restoration. */
  std::string error;
};

/**
 * Queries the complete live routing snapshot.
 * @param socketPath Daemon endpoint.
 * @return Status and JSON, or a diagnostic with confirmed offline classification.
 */
OutputStatusQueryResult queryOutputStatus(const std::filesystem::path &socketPath);

/**
 * Selects devices while preserving all previous slot numbers and labels.
 * @param previous Valid retained routing choices.
 * @param inventory Current physical output descriptions.
 * @param nodeNames Unique current node names to enable, in initial allocation order.
 * @return Multiple-mode configuration, or the unchanged previous choices and error.
 * @remarks Changed profiles require explicit reassignment through a full configuration.
 */
OutputConfigurationResult selectOutputDevices(const OutputConfiguration &previous,
    std::span<const AvailableOutput> inventory, std::span<const std::string> nodeNames);

/**
 * Applies a guarded live change and atomically saves its output choices.
 * @param options Resolved daemon and persistence endpoints.
 * @param change User intent to resolve against the live or offline snapshot.
 * @return Confirmed outcome; save failure attempts revision-guarded restoration.
 * @remarks A missing/refusing endpoint permits offline persistence. Uncertain
 * transport failures never count as offline success. Other startup choices are retained.
 */
PersistentOutputResult executeOutputChange(const PersistentOutputOptions &options, const OutputChange &change);

/**
 * Formats every final EffeTune slot and its physical destination.
 * @param status Valid complete daemon status.
 * @return Human-readable mode, mappings, labels, and device presence ending in newline.
 * @remarks Device presence does not certify active audio transport or delay compensation.
 */
std::string formatOutputStatus(const ControlRuntimeStatus &status);

} // namespace pipetune

#endif
