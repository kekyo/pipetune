#include "application-state.h"

namespace pipetune_gtk {

constexpr auto kMinimumInputRateIntervalMilliseconds = std::int64_t{500};

static InputRateState initialInputRateState() {
  return {
      .hasBaseline = false,
      .baselineFrames = 0,
      .baselineMonotonicMilliseconds = 0,
      .baselineSampleFormat = {},
      .baselineSampleRate = 0,
      .baselineChannelCount = 0,
      .hasRate = false,
      .framesPerSecond = 0.0,
  };
}

static bool inputFormatMatches(const InputRateState &inputRate,
                               const pipetune::ControlRuntimeStatus &status) {
  return inputRate.baselineSampleFormat == status.inputSampleFormat &&
         inputRate.baselineSampleRate == status.inputSampleRate &&
         inputRate.baselineChannelCount == status.inputChannelCount;
}

static void establishInputRateBaseline(
    InputRateState &inputRate,
    const pipetune::ControlRuntimeStatus &status,
    std::int64_t receivedAtMonotonicMilliseconds) {
  inputRate.hasBaseline = true;
  inputRate.baselineFrames = status.inputFramesReceived;
  inputRate.baselineMonotonicMilliseconds =
      receivedAtMonotonicMilliseconds;
  inputRate.baselineSampleFormat = status.inputSampleFormat;
  inputRate.baselineSampleRate = status.inputSampleRate;
  inputRate.baselineChannelCount = status.inputChannelCount;
  inputRate.hasRate = false;
  inputRate.framesPerSecond = 0.0;
}

static void updateInputRate(
    InputRateState &inputRate,
    const pipetune::ControlRuntimeStatus &status,
    std::int64_t receivedAtMonotonicMilliseconds) {
  if (status.inputSampleFormat.empty()) {
    inputRate = initialInputRateState();
    return;
  }
  if (!inputRate.hasBaseline ||
      !inputFormatMatches(inputRate, status) ||
      status.inputFramesReceived < inputRate.baselineFrames ||
      receivedAtMonotonicMilliseconds <
          inputRate.baselineMonotonicMilliseconds) {
    establishInputRateBaseline(inputRate, status,
                               receivedAtMonotonicMilliseconds);
    return;
  }

  const auto elapsedMilliseconds =
      receivedAtMonotonicMilliseconds -
      inputRate.baselineMonotonicMilliseconds;
  if (elapsedMilliseconds <
      kMinimumInputRateIntervalMilliseconds) {
    return;
  }
  const auto frames =
      status.inputFramesReceived - inputRate.baselineFrames;
  inputRate.framesPerSecond =
      static_cast<double>(frames) * 1000.0 /
      static_cast<double>(elapsedMilliseconds);
  inputRate.hasRate = true;
  inputRate.baselineFrames = status.inputFramesReceived;
  inputRate.baselineMonotonicMilliseconds =
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
              .preferredTarget = {},
              .selectedTarget = {},
              .outputSelectionReason =
                  pipetune::ControlOutputSelectionReason::unavailable,
              .availableOutputs = {},
              .defaultSinkActive = false,
              .overrunFrames = 0,
              .underrunFrames = 0,
              .processingErrors = 0,
              .inputSampleFormat = {},
              .inputSampleRate = 0,
              .inputChannelCount = 0,
              .inputFramesReceived = 0,
              .inputLastReceivedUnixMilliseconds = 0,
          },
      .warnings = {},
      .diagnostic = {},
      .operationPending = false,
      .inputRate = initialInputRateState(),
  };
}

void markControlConnecting(ApplicationState &state) {
  state.connection = ControlConnectionState::connecting;
  state.hasRuntimeStatus = false;
  state.diagnostic.clear();
  state.inputRate = initialInputRateState();
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
  updateInputRate(state.inputRate, response.status,
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
  state.inputRate = initialInputRateState();
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
      !state.runtime.defaultSinkActive ||
      state.runtime.selectedTarget.empty() ||
      state.runtime.overrunFrames != 0 ||
      state.runtime.underrunFrames != 0 ||
      state.runtime.processingErrors != 0) {
    return TrayVisualState::attention;
  }
  return TrayVisualState::active;
}

} // namespace pipetune_gtk
