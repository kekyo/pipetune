#ifndef PIPETUNE_PIPEWIRE_PIPELINE_H
#define PIPETUNE_PIPEWIRE_PIPELINE_H

#include "pipetune/dsp_pipeline.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>

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
  /** Target sink node name or object serial; empty selects the current default. */
  std::string targetObject;
  /** Initial preset path, or empty when the supplied pipeline is bypass. */
  std::filesystem::path initialPresetPath;
  /** Startup configuration diagnostic reported until a live mode change. */
  std::string initialConfigurationError;
  /** User-only control socket path, or empty to disable live control. */
  std::filesystem::path controlSocketPath;
  /** Fixed stream sample rate in hertz, from 32000 through 192000. */
  std::uint32_t sampleRate;
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

} // namespace pipetune

#endif
