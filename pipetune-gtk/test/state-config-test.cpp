#include "application-state.h"

#include "pipetune/control_protocol.h"

#include <array>
#include <cmath>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool approximately(double actual, double expected) {
  return std::abs(actual - expected) < 0.000001;
}

static pipetune::ControlFilterOutputStatus filterOutput(
    pipetune::ControlFilterState state =
        pipetune::ControlFilterState::active) {
  const auto filtered =
      state == pipetune::ControlFilterState::active ||
      state == pipetune::ControlFilterState::waiting;
  return {
      .targetNodeName = "alsa_output.speaker",
      .targetDescription = "Speakers",
      .filterNodeName = filtered ? "pipetune.filter.speaker" : "",
      .state = state,
      .error = state == pipetune::ControlFilterState::error
                   ? "filter failed open"
                   : "",
      .channelCount = filtered ? 2u : 0u,
      .sampleRateCapabilities =
          {.known = true,
           .constraints =
               {{.kind = pipetune::SampleRateConstraintKind::discrete,
                 .minimum = 48000,
                 .maximum = 48000,
                 .step = 0}}},
      .dspSampleRate = filtered ? 48000u : 0u,
      .outputSampleRate = filtered ? 48000u : 0u,
      .activeOutputSampleRate = filtered ? 48000u : 0u,
      .rateFallback = false,
      .latencyFrames = filtered ? 64u : 0u,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
  };
}

static pipetune::ControlRuntimeStatus runtimeStatus(
    pipetune::ProcessingMode mode = pipetune::ProcessingMode::preset,
    pipetune::ControlFilterState filterState =
        pipetune::ControlFilterState::active,
    std::string configurationError = {}) {
  return {
      .processingMode = mode,
      .activePreset = mode == pipetune::ProcessingMode::preset
                          ? "/tmp/active.effetune_preset"
                          : "",
      .configurationError = std::move(configurationError),
      .activePluginCount =
          mode == pipetune::ProcessingMode::preset ? 4u : 0u,
      .policyBackend = "wireplumber-0.5",
      .filterOutputs = {filterOutput(filterState)},
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .dspProcessedFrames = 0,
      .dspProcessingNanoseconds = 0,
  };
}

static pipetune::ControlResponseParseResult responseFor(
    const pipetune::ControlRuntimeStatus &status,
    std::span<const pipetune::ControlWarning> warnings = {}) {
  return pipetune::parseControlResponse(
      pipetune::makeControlSuccessResponse(status, warnings));
}

static pipetune::ControlResponseParseResult dspStatusEvent(
    std::uint64_t processedFrames,
    std::uint64_t processingNanoseconds,
    std::uint64_t overrunFrames,
    std::uint64_t underrunFrames,
    std::uint64_t processingErrors) {
  auto status = runtimeStatus();
  status.dspProcessedFrames = processedFrames;
  status.dspProcessingNanoseconds = processingNanoseconds;
  status.overrunFrames = overrunFrames;
  status.underrunFrames = underrunFrames;
  status.processingErrors = processingErrors;
  return pipetune::parseControlResponse(
      pipetune::makeControlStatusEvent(status));
}

static bool testApplicationState() {
  auto state = pipetune_gtk::initialApplicationState();
  if (!check(state.connection ==
                 pipetune_gtk::ControlConnectionState::disconnected,
             "initial connection state differs") ||
      !check(!pipetune_gtk::isPresetApplied(state),
             "initial state must not confirm an applied preset") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::disconnected,
             "initial tray state differs")) {
    return false;
  }

  pipetune_gtk::markControlConnecting(state);
  const auto healthy = responseFor(runtimeStatus());
  pipetune_gtk::applyControlResponse(state, healthy, 1000);
  if (!check(state.connection ==
                 pipetune_gtk::ControlConnectionState::connected &&
                 state.hasRuntimeStatus &&
                 state.runtime.activePluginCount == 4,
             "valid status must connect and retain runtime state") ||
      !check(pipetune_gtk::isPresetApplied(state),
             "preset runtime must confirm an applied preset") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::active,
             "healthy filtered outputs must use the active tray state")) {
    return false;
  }

  auto bypassState = state;
  pipetune_gtk::applyControlResponse(
      bypassState,
      responseFor(runtimeStatus(pipetune::ProcessingMode::bypass)), 2000);
  if (!check(!pipetune_gtk::isPresetApplied(bypassState),
             "bypass runtime must not confirm an applied preset")) {
    return false;
  }

  const auto warnings = std::array<pipetune::ControlWarning, 1>{
      pipetune::ControlWarning{.nodeIndex = 2,
                               .pluginName = "Unavailable DSP",
                               .reason = "not built"}};
  pipetune_gtk::applyControlResponse(
      state, responseFor(runtimeStatus(), warnings), 3000);
  if (!check(state.warnings.size() == 1,
             "ordinary response warnings were not retained") ||
      !check(pipetune_gtk::trayVisualState(state) ==
                 pipetune_gtk::TrayVisualState::attention,
             "warnings must request attention")) {
    return false;
  }
  const auto event = pipetune::parseControlResponse(
      pipetune::makeControlStatusEvent(runtimeStatus()));
  pipetune_gtk::applyControlResponse(state, event, 4000);
  if (!check(state.warnings.size() == 1,
             "status events must not erase request warnings")) {
    return false;
  }
  pipetune_gtk::clearControlNotice(state);

  auto degraded = state;
  pipetune_gtk::applyControlResponse(
      degraded,
      responseFor(runtimeStatus(
          pipetune::ProcessingMode::preset,
          pipetune::ControlFilterState::bypassed)),
      5000);
  if (!check(pipetune_gtk::trayVisualState(degraded) ==
                 pipetune_gtk::TrayVisualState::attention,
             "a direct fail-open route must request attention")) {
    return false;
  }

  auto noOutputs = runtimeStatus();
  noOutputs.filterOutputs.clear();
  pipetune_gtk::applyControlResponse(
      degraded, responseFor(noOutputs), 6000);
  if (!check(pipetune_gtk::trayVisualState(degraded) ==
                 pipetune_gtk::TrayVisualState::attention,
             "no physical outputs must request attention")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      degraded,
      responseFor(runtimeStatus(
          pipetune::ProcessingMode::preset,
          pipetune::ControlFilterState::active,
          "configured preset is unavailable")),
      7000);
  if (!check(pipetune_gtk::trayVisualState(degraded) ==
                 pipetune_gtk::TrayVisualState::attention,
             "a startup configuration error must request attention")) {
    return false;
  }

  auto backendFallback = state;
  backendFallback.runtime.dspBackendFallback = true;
  backendFallback.runtime.dspBackendError = "SIMD backend unavailable";
  if (!check(pipetune_gtk::trayVisualState(backendFallback) ==
                 pipetune_gtk::TrayVisualState::attention,
             "a DSP backend fallback must request attention")) {
    return false;
  }

  pipetune_gtk::markControlDisconnected(state, "daemon stopped");
  return check(state.connection ==
                   pipetune_gtk::ControlConnectionState::disconnected &&
                   state.diagnostic == "daemon stopped" &&
                   !pipetune_gtk::isPresetApplied(state),
               "disconnect state differs");
}

static bool testPeriodicRuntimeMeasurements() {
  auto state = pipetune_gtk::initialApplicationState();
  pipetune_gtk::applyControlResponse(
      state, dspStatusEvent(48000, 96000000, 1, 2, 3), 1000);
  if (!check(state.dspTiming.hasBaseline &&
                 !state.dspTiming.hasAverage,
             "first DSP status must establish a timing baseline")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, dspStatusEvent(96000, 216000000, 4, 5, 6), 2000);
  if (!check(state.dspTiming.hasAverage &&
                 approximately(state.dspTiming.nanosecondsPerFrame,
                               2500.0) &&
                 approximately(state.dspTiming.loadPercent, 12.0),
             "periodic aggregate DSP timing differs") ||
      !check(state.runtime.overrunFrames == 4 &&
                 state.runtime.underrunFrames == 5 &&
                 state.runtime.processingErrors == 6,
             "periodic runtime counters were not refreshed")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, dspStatusEvent(96000, 216000000, 4, 5, 6), 3000);
  if (!check(!state.dspTiming.hasAverage &&
                 state.dspTiming.loadPercent == 0.0,
             "an interval without frames must clear stale timing")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state, dspStatusEvent(100, 1000, 0, 0, 0), 4000);
  if (!check(state.dspTiming.hasBaseline &&
                 !state.dspTiming.hasAverage &&
                 state.dspTiming.baselineFrames == 100,
             "counter regression must establish a new baseline")) {
    return false;
  }

  pipetune_gtk::applyControlResponse(
      state,
      responseFor(runtimeStatus(pipetune::ProcessingMode::bypass)), 8000);
  return check(!state.dspTiming.hasBaseline &&
                   !state.dspTiming.hasAverage,
               "bypass must clear DSP timing measurements");
}

int main() {
  return testApplicationState() && testPeriodicRuntimeMeasurements() ? 0 : 1;
}
