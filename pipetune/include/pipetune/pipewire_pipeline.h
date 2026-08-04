#ifndef PIPETUNE_PIPEWIRE_PIPELINE_H
#define PIPETUNE_PIPEWIRE_PIPELINE_H

#include "pipetune/dsp_pipeline.h"
#include "pipetune/sample_rate.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace pipetune {

/**
 * Receives a one-shot notification after both PipeWire streams are ready.
 *
 * The callback runs from the PipeWire main-loop context, not a real-time
 * process callback.
 *
 * @param userData Opaque pointer from PipeWirePipelineOptions.
 */
using PipeWireReadyCallback = void (*)(void *userData);

/**
 * Configures the PipeWire nodes surrounding one native DSP pipeline.
 */
struct PipeWirePipelineOptions {
  /** Stable PipeWire node name exposed as the virtual system sink. */
  std::string sinkName;
  /** Human-readable virtual sink description. */
  std::string sinkDescription;
  /**
   * Preferred sink node.name; empty follows the physical system default.
   *
   * A missing preference falls back to the physical system default and is
   * restored automatically when the preferred node returns.
   */
  std::string targetObject;
  /** Initial preset path, or empty when the supplied pipeline is bypass. */
  std::filesystem::path initialPresetPath;
  /** Startup configuration diagnostic reported until a live mode change. */
  std::string initialConfigurationError;
  /** User-only control socket path, or empty to disable live control. */
  std::filesystem::path controlSocketPath;
  /** Initial capture, playback media-format, and DSP rate in hertz. */
  std::uint32_t dspSampleRate;
  /** Initial PipeWire playback graph-rate hint in hertz. */
  std::uint32_t outputSampleRate;
  /** Initial Max/fixed and suggest/force policy. */
  SampleRatePolicy ratePolicy;
  /** Fixed planar channel count, from one through eight. */
  std::uint32_t channelCount;
  /** Largest DSP block processed in one call; must be at least 32. */
  std::uint32_t maxFrames;
  /** Inter-stream ring capacity; must be at least maxFrames. */
  std::uint32_t ringCapacityFrames;
  /**
   * True to make the virtual sink the effective system default for the run.
   *
   * The configured session-manager default is not changed. An orderly signal
   * restores the selected physical sink before the function returns.
   */
  bool manageDefaultSink;
  /** One-shot readiness callback, or null when no notification is needed. */
  PipeWireReadyCallback readyCallback;
  /** Opaque argument passed to readyCallback. */
  void *readyUserData;
  /**
   * Scalar and SIMD backends discovered for startup and live switching.
   *
   * When neither result was supplied, runPipeWirePipeline discovers both
   * executable-relative backends before starting the runtime.
   */
  DspBackends dspBackends = {};
  /** Initial persisted DSP backend choice. */
  DspBackendKind configuredDspBackend = DspBackendKind::scalar;
  /** Initial persisted automatic or pinned SIMD dispatch preference. */
  DspSimdVariant configuredDspSimdVariant =
      DspSimdVariant::automatic;
};

/**
 * Selects when the PipeWire main loop returns successfully.
 */
enum class PipeWireRunMode {
  /** Process audio until SIGINT, SIGTERM, or a PipeWire error. */
  untilInterrupted,
  /** Return after both streams have completed PipeWire format negotiation. */
  untilReady
};

/**
 * Reports the outcome and real-time bridge counters for one run.
 */
struct PipeWireRunResult {
  /** True after an orderly signal or successful readiness check. */
  bool success;
  /** Fatal validation, connection, negotiation, or processing diagnostic. */
  std::string error;
  /** Input frames discarded because the bridge was full. */
  std::uint64_t overrunFrames;
  /** Output frames replaced by silence because the bridge was empty. */
  std::uint64_t underrunFrames;
  /** DSP blocks that could not be processed and were passed through. */
  std::uint64_t processingErrors;
  /** Last physical node.name selected for playback, or empty when unavailable. */
  std::string selectedTarget;
};

/** Describes the fail-open state of one physical output filter. */
enum class PipeWireFilterOutputState {
  /** Stream nodes are still negotiating or awaiting policy. */
  waiting,
  /** WirePlumber policy has enabled the ready filter. */
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
  /** Hidden PipeTune main node.name. */
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
 * Publishes a virtual sink and forwards its processed PCM to a real sink.
 *
 * The supplied DSP pipeline must have been prepared for the same sample rate,
 * at least channelCount channels, and at least maxFrames frames. Ownership is
 * retained for the complete run so control requests can replace the pipeline.
 * This function blocks according to mode and does not allocate in PipeWire
 * process callbacks.
 *
 * @param pipeline Owned prepared native EffeTune pipeline.
 * @param options PipeWire stream and bridge configuration.
 * @param mode Main-loop completion condition.
 * @return Completion status and bridge counters.
 */
PipeWireRunResult runPipeWirePipeline(std::unique_ptr<DspPipeline> pipeline,
                                      const PipeWirePipelineOptions &options,
                                      PipeWireRunMode mode);

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
