#ifndef PIPETUNE_TRANSPARENT_FILTER_SERVICE_H
#define PIPETUNE_TRANSPARENT_FILTER_SERVICE_H

#include "pipetune/dsp_pipeline.h"
#include "pipetune/sample_rate.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pipetune {

/** Receives a one-shot notification after initial PipeWire setup settles. */
using PipeWireReadyCallback = void (*)(void *userData);

/** Selects when the transparent-filter main loop returns successfully. */
enum class PipeWireRunMode {
  /** Process audio until SIGINT, SIGTERM, or a PipeWire error. */
  untilInterrupted,
  /** Return after initial output enumeration and filter negotiation. */
  untilReady
};

/** Describes the fail-open state of one physical output filter. */
enum class PipeWireFilterOutputState {
  /** Stream nodes are still negotiating or awaiting policy. */
  waiting,
  /** WirePlumber policy has enabled the ready PipeTune filter. */
  active,
  /** The physical output remains directly routed without PipeTune. */
  bypassed,
  /** This output could not create or negotiate its filter runtime. */
  error
};

/** Configures the transparent-filter service shared by all physical outputs. */
struct PipeWireFilterServiceOptions {
  /** Initial preset path, or empty when the supplied recipe is bypass. */
  std::filesystem::path initialPresetPath;
  /** Startup configuration diagnostic reported until a live mode change. */
  std::string initialConfigurationError;
  /** User-only control socket path, or empty to disable live control. */
  std::filesystem::path controlSocketPath;
  /** Global Max/fixed and suggest/force policy resolved per output. */
  SampleRatePolicy ratePolicy;
  /** Largest DSP block processed in one call; must be at least 32. */
  std::uint32_t maxFrames;
  /** Per-output inter-stream ring capacity; must be at least maxFrames. */
  std::uint32_t ringCapacityFrames;
  /** One-shot callback after enumeration and all initial filters settle. */
  PipeWireReadyCallback readyCallback;
  /** Opaque argument passed to readyCallback. */
  void *readyUserData;
  /** Scalar and SIMD backends available for live runtime changes. */
  DspBackends dspBackends = {};
  /** Initial persisted DSP backend choice. */
  DspBackendKind configuredDspBackend = DspBackendKind::scalar;
  /** Initial persisted automatic or pinned SIMD dispatch preference. */
  DspSimdVariant configuredDspSimdVariant = DspSimdVariant::automatic;
};

/** Reports the current runtime for one physical PipeWire output. */
struct PipeWireFilterOutputStatus {
  /** Physical sink node.name that remains visible to the desktop. */
  std::string targetNodeName;
  /** User-facing physical sink description. */
  std::string targetDescription;
  /** Internal PipeTune main node.name. */
  std::string filterNodeName;
  /** Filter activation or fail-open state. */
  PipeWireFilterOutputState state;
  /** Output-specific runtime or policy diagnostic. */
  std::string error;
  /** Exact processed channel count. */
  std::uint32_t channelCount;
  /** Output-specific EffeTune processing rate. */
  std::uint32_t dspSampleRate;
  /** Output-specific PipeWire graph-rate hint. */
  std::uint32_t outputSampleRate;
  /** Native DSP latency published to PipeWire, in frames. */
  std::uint32_t latencyFrames;
};

/** Reports transparent-filter service completion and aggregate counters. */
struct PipeWireFilterServiceResult {
  /** True after readiness or an orderly signal. */
  bool success;
  /** Fatal service-level validation or PipeWire diagnostic. */
  std::string error;
  /** WirePlumber backend from the runtime handshake, or empty. */
  std::string policyBackend;
  /** Final filter or direct-route status of every observed output sink. */
  std::vector<PipeWireFilterOutputStatus> outputs;
  /** Aggregate input frames discarded by per-output bridges. */
  std::uint64_t overrunFrames;
  /** Aggregate output frames replaced by silence. */
  std::uint64_t underrunFrames;
  /** Aggregate DSP blocks passed through after processing errors. */
  std::uint64_t processingErrors;
};

/**
 * Runs one hidden target-specific DSP filter for every eligible physical sink.
 *
 * The supplied pipeline retains the parsed preset recipe. A fresh pipeline is
 * rebuilt for each output's exact rate and one-through-eight-channel layout.
 * Output additions, format changes, and removals update only their associated
 * runtime. Unsupported outputs are left on WirePlumber's direct route.
 *
 * @param pipeline Owned prepared recipe used to construct output runtimes.
 * @param options Shared service and per-output bridge limits.
 * @param mode Main-loop completion condition.
 * @return Completion state, output statuses, and aggregate counters.
 */
PipeWireFilterServiceResult runPipeWireFilterService(
    std::unique_ptr<DspPipeline> pipeline,
    const PipeWireFilterServiceOptions &options,
    PipeWireRunMode mode);

} // namespace pipetune

#endif
