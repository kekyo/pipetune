#include "application-state.h"

#include <algorithm>

namespace pipetune_gtk {

static DspTimingState initialDspTimingState() {
  return {
      .hasBaseline = false,
      .baselineFrames = 0,
      .baselineNanoseconds = 0,
      .baselineMonotonicMilliseconds = 0,
      .hasAverage = false,
      .nanosecondsPerFrame = 0.0,
      .loadPercent = 0.0,
  };
}

static void establishDspTimingBaseline(
    DspTimingState &timing,
    const pipetune::ControlRuntimeStatus &status,
    std::int64_t receivedAtMonotonicMilliseconds) {
  timing.hasBaseline = true;
  timing.baselineFrames = status.dspProcessedFrames;
  timing.baselineNanoseconds = status.dspProcessingNanoseconds;
  timing.baselineMonotonicMilliseconds =
      receivedAtMonotonicMilliseconds;
  timing.hasAverage = false;
  timing.nanosecondsPerFrame = 0.0;
  timing.loadPercent = 0.0;
}

static void updateDspTiming(
    DspTimingState &timing,
    const pipetune::ControlRuntimeStatus &status,
    std::int64_t receivedAtMonotonicMilliseconds) {
  if (status.processingMode != pipetune::ProcessingMode::preset) {
    timing = initialDspTimingState();
    return;
  }
  if (!timing.hasBaseline ||
      status.dspProcessedFrames < timing.baselineFrames ||
      status.dspProcessingNanoseconds < timing.baselineNanoseconds ||
      receivedAtMonotonicMilliseconds <=
          timing.baselineMonotonicMilliseconds) {
    establishDspTimingBaseline(
        timing, status, receivedAtMonotonicMilliseconds);
    return;
  }

  const auto processedFrames =
      status.dspProcessedFrames - timing.baselineFrames;
  const auto processingNanoseconds =
      status.dspProcessingNanoseconds - timing.baselineNanoseconds;
  const auto elapsedMilliseconds =
      receivedAtMonotonicMilliseconds -
      timing.baselineMonotonicMilliseconds;
  if (processedFrames != 0) {
    timing.nanosecondsPerFrame =
        static_cast<double>(processingNanoseconds) /
        static_cast<double>(processedFrames);
    timing.hasAverage = true;
    timing.loadPercent =
        static_cast<double>(processingNanoseconds) /
        (static_cast<double>(elapsedMilliseconds) * 1'000'000.0) * 100.0;
  } else {
    timing.hasAverage = false;
    timing.nanosecondsPerFrame = 0.0;
    timing.loadPercent = 0.0;
  }
  timing.baselineFrames = status.dspProcessedFrames;
  timing.baselineNanoseconds = status.dspProcessingNanoseconds;
  timing.baselineMonotonicMilliseconds =
      receivedAtMonotonicMilliseconds;
}

ApplicationState initialApplicationState() {
  return {
      .connection = ControlConnectionState::disconnected,
      .hasRuntimeStatus = false,
      .runtime =
          {
              .processingMode = pipetune::ProcessingMode::bypass,
              .activePreset = {},
              .configurationError = {},
              .activePluginCount = 0,
              .overrunFrames = 0,
              .underrunFrames = 0,
              .processingErrors = 0,
              .dspProcessedFrames = 0,
              .dspProcessingNanoseconds = 0,
              .configuredRatePolicy = pipetune::defaultSampleRatePolicy(),
          },
      .warnings = {},
      .diagnostic = {},
      .operationPending = false,
      .dspTiming = initialDspTimingState(),
  };
}

void markControlConnecting(ApplicationState &state) {
  state.connection = ControlConnectionState::connecting;
  state.hasRuntimeStatus = false;
  state.diagnostic.clear();
  state.dspTiming = initialDspTimingState();
}

void applyControlResponse(
    ApplicationState &state,
    const pipetune::ControlResponseParseResult &response,
    std::int64_t receivedAtMonotonicMilliseconds) {
  if (!response.valid) {
    state.diagnostic = response.error;
    return;
  }
  state.connection = ControlConnectionState::connected;
  if (!response.success) {
    state.diagnostic = response.error;
    return;
  }

  state.hasRuntimeStatus = true;
  state.runtime = response.status;
  updateDspTiming(state.dspTiming, response.status,
                  receivedAtMonotonicMilliseconds);
  if (response.kind == pipetune::ControlResponseKind::response) {
    state.warnings = response.warnings;
    state.diagnostic.clear();
  }
}

void markControlDisconnected(ApplicationState &state,
                             std::string_view diagnostic) {
  state.connection = ControlConnectionState::disconnected;
  state.operationPending = false;
  state.diagnostic = diagnostic;
  state.dspTiming = initialDspTimingState();
}

void setControlOperationPending(ApplicationState &state, bool pending) {
  state.operationPending = pending;
}

void setControlDiagnostic(ApplicationState &state,
                          std::string_view diagnostic) {
  state.diagnostic = diagnostic;
}

void clearControlNotice(ApplicationState &state) {
  state.warnings.clear();
  state.diagnostic.clear();
}

bool isPresetApplied(const ApplicationState &state) {
  return state.connection == ControlConnectionState::connected &&
         state.hasRuntimeStatus &&
         state.runtime.processingMode == pipetune::ProcessingMode::preset &&
         !state.runtime.activePreset.empty();
}

TrayVisualState trayVisualState(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected) {
    return TrayVisualState::disconnected;
  }
  if (!state.hasRuntimeStatus || !state.diagnostic.empty() ||
      !state.warnings.empty() ||
      !state.runtime.configurationError.empty() ||
      state.runtime.dspBackendFallback ||
      !state.runtime.dspBackendError.empty() ||
      state.runtime.filterOutputs.empty() ||
      std::any_of(state.runtime.filterOutputs.begin(),
                  state.runtime.filterOutputs.end(),
                  [](const auto &output) {
                    return output.state !=
                           pipetune::ControlFilterState::active;
                  }) ||
      state.runtime.overrunFrames != 0 ||
      state.runtime.underrunFrames != 0 ||
      state.runtime.processingErrors != 0) {
    return TrayVisualState::attention;
  }
  return TrayVisualState::active;
}

} // namespace pipetune_gtk
