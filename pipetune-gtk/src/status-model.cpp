#include "status-model.h"

#include "status-text.h"

#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_idle.h"
#include "pipetune/sample_rate.h"

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pipetune_gtk {

constexpr auto kNanosecondsPerSecond = 1'000'000'000.0;

static StatusItem textItem(std::string id, std::string label,
                           std::string value,
                           StatusSeverity severity =
                               StatusSeverity::normal,
                           std::string tooltip = {}) {
  return {
      .id = std::move(id),
      .label = std::move(label),
      .value = std::move(value),
      .numericValue = std::nullopt,
      .unit = {},
      .severity = severity,
      .displayKind = StatusDisplayKind::text,
      .minimum = std::nullopt,
      .maximum = std::nullopt,
      .tooltip = std::move(tooltip),
  };
}

static StatusItem numericTextItem(
    std::string id, std::string label, std::string value,
    double numericValue, std::string unit, double minimum,
    double maximum, StatusSeverity severity = StatusSeverity::normal) {
  return {
      .id = std::move(id),
      .label = std::move(label),
      .value = std::move(value),
      .numericValue = numericValue,
      .unit = std::move(unit),
      .severity = severity,
      .displayKind = StatusDisplayKind::text,
      .minimum = minimum,
      .maximum = maximum,
      .tooltip = {},
  };
}

static std::string fixedDecimal(double value, int precision) {
  auto stream = std::ostringstream{};
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

static std::string connectionText(ControlConnectionState connection) {
  switch (connection) {
  case ControlConnectionState::disconnected:
    return "Disconnected";
  case ControlConnectionState::connecting:
    return "Connecting";
  case ControlConnectionState::connected:
    return "Connected";
  }
  return "Unknown";
}

static std::string yesNo(bool value) {
  return value ? "Yes" : "No";
}

static std::string processingModeText(pipetune::ProcessingMode mode) {
  switch (mode) {
  case pipetune::ProcessingMode::bypass:
    return "Bypass";
  case pipetune::ProcessingMode::preset:
    return "Preset";
  }
  return "Unknown";
}

static std::string configuredProcessingText(
    const pipetune::StartupConfig &config) {
  return config.presetFound ? "Preset" : "Bypass";
}

static std::string shortPath(const std::filesystem::path &path) {
  if (path.empty()) {
    return "—";
  }
  const auto filename = path.filename().string();
  return filename.empty() ? path.string() : filename;
}

static std::string sampleRateText(std::uint32_t rate) {
  if (rate == 0) {
    return "—";
  }
  const auto kilohertz = static_cast<double>(rate) / 1000.0;
  auto text = fixedDecimal(kilohertz, rate % 1000 == 0 ? 0 : 1);
  return text + " kHz";
}

static std::string sampleRateModeText(pipetune::SampleRateMode mode) {
  switch (mode) {
  case pipetune::SampleRateMode::maximum:
    return "Maximum";
  case pipetune::SampleRateMode::fixed:
    return "Fixed";
  }
  return "Unknown";
}

static std::string rateEnforcementText(
    pipetune::SampleRateEnforcement enforcement) {
  switch (enforcement) {
  case pipetune::SampleRateEnforcement::suggest:
    return "Suggest";
  case pipetune::SampleRateEnforcement::force:
    return "Force";
  }
  return "Unknown";
}

static std::string backendText(pipetune::DspBackendKind backend) {
  switch (backend) {
  case pipetune::DspBackendKind::scalar:
    return "Scalar";
  case pipetune::DspBackendKind::simd:
    return "SIMD";
  }
  return "Unknown";
}

static std::string simdVariantText(pipetune::DspSimdVariant variant) {
  const auto name = pipetune::dspSimdVariantName(variant);
  return name.empty() ? "Unknown" : std::string(name);
}

static std::string effectiveVariantText(
    const std::optional<pipetune::DspBackendVariant> &variant) {
  if (!variant.has_value()) {
    return "Unavailable";
  }
  const auto name = pipetune::dspBackendVariantName(*variant);
  return name.empty() ? "Unknown" : std::string(name);
}

static std::string idlePolicyText(pipetune::DspIdlePolicy policy) {
  const auto name = pipetune::dspIdlePolicyName(policy);
  if (name == "conservative") {
    return "Conservative";
  }
  if (name == "exact") {
    return "Exact";
  }
  return "Unknown";
}

static std::string idleStateText(pipetune::DspIdleState state) {
  const auto name = pipetune::dspIdleStateName(state);
  if (name == "active") {
    return "Active";
  }
  if (name == "sleeping") {
    return "Sleeping";
  }
  return name.empty() ? "Unknown" : std::string(name);
}

static std::string selectionReasonText(
    pipetune::ControlOutputSelectionReason reason) {
  switch (reason) {
  case pipetune::ControlOutputSelectionReason::unavailable:
    return "No output available";
  case pipetune::ControlOutputSelectionReason::systemDefault:
    return "System default";
  case pipetune::ControlOutputSelectionReason::preferred:
    return "Preferred output";
  case pipetune::ControlOutputSelectionReason::fallback:
    return "System-default fallback";
  }
  return "Unknown";
}

static const pipetune::ControlOutputDevice *findOutput(
    const pipetune::ControlRuntimeStatus &runtime,
    std::string_view name) {
  for (const auto &output : runtime.availableOutputs) {
    if (output.name == name) {
      return &output;
    }
  }
  return nullptr;
}

static StatusItem outputItem(
    std::string id, std::string label,
    const pipetune::ControlRuntimeStatus &runtime,
    std::string_view name, StatusSeverity severity) {
  if (name.empty()) {
    return textItem(std::move(id), std::move(label), "Unavailable",
                    severity);
  }
  const auto *output = findOutput(runtime, name);
  if (output == nullptr) {
    return textItem(std::move(id), std::move(label), std::string(name),
                    severity, std::string(name));
  }
  const auto value =
      output->description.empty() ? output->name : output->description;
  auto tooltip = value;
  if (value != output->name) {
    tooltip += "\n" + output->name;
  }
  return textItem(std::move(id), std::move(label), value, severity,
                  std::move(tooltip));
}

static StatusItem dspLoadItem(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected ||
      !state.hasRuntimeStatus || state.runtime.pipeWireIdle ||
      state.runtime.dspIdleState == pipetune::DspIdleState::sleeping ||
      !state.dspTiming.hasAverage ||
      !std::isfinite(state.dspTiming.nanosecondsPerFrame) ||
      state.dspTiming.nanosecondsPerFrame < 0.0 ||
      state.runtime.inputSampleRate == 0) {
    return textItem("dsp.load", "Load", "—");
  }
  const auto frameBudget =
      kNanosecondsPerSecond /
      static_cast<double>(state.runtime.inputSampleRate);
  const auto load =
      state.dspTiming.nanosecondsPerFrame / frameBudget * 100.0;
  return numericTextItem(
      "dsp.load", "Load", fixedDecimal(load, 1) + "%", load, "%",
      0.0, 100.0,
      load > 100.0 ? StatusSeverity::error : StatusSeverity::normal);
}

static StatusItem dspTimeItem(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected ||
      !state.hasRuntimeStatus || state.runtime.pipeWireIdle ||
      state.runtime.dspIdleState == pipetune::DspIdleState::sleeping ||
      !state.dspTiming.hasAverage ||
      !std::isfinite(state.dspTiming.nanosecondsPerFrame) ||
      state.dspTiming.nanosecondsPerFrame < 0.0) {
    return textItem("dsp.processing-time", "Processing time", "—");
  }
  const auto microseconds =
      state.dspTiming.nanosecondsPerFrame / 1000.0;
  return numericTextItem(
      "dsp.processing-time", "Processing time",
      fixedDecimal(microseconds, 2) + " µs/frame", microseconds,
      "µs/frame", 0.0, microseconds, StatusSeverity::normal);
}

static StatusSeverity nonzeroSeverity(std::uint64_t value) {
  return value == 0 ? StatusSeverity::normal : StatusSeverity::warning;
}

static std::string errorText(std::string_view error) {
  return error.empty() ? "None" : std::string(error);
}

static StatusSeverity errorSeverity(std::string_view error) {
  return error.empty() ? StatusSeverity::normal : StatusSeverity::error;
}

std::vector<StatusSection> buildStatusSections(
    const ApplicationState &state,
    const pipetune::StartupConfig &saved,
    std::uint64_t currentUnixMilliseconds) {
  const auto connected =
      state.connection == ControlConnectionState::connected &&
      state.hasRuntimeStatus;
  const auto unavailable = connected ? StatusSeverity::normal
                                     : StatusSeverity::warning;
  const auto input =
      inputStatusText(state, currentUnixMilliseconds);
  const auto livePreset =
      std::filesystem::path(state.runtime.activePreset);
  const auto savedPreset = saved.presetPath;
  const auto savedOutput =
      saved.preferredOutputFound ? saved.preferredOutput : "System default";
  const auto livePreference =
      state.runtime.preferredTarget.empty()
          ? std::string("System default")
          : state.runtime.preferredTarget;
  const auto fixedSavedRate =
      saved.ratePolicy.mode == pipetune::SampleRateMode::fixed
          ? sampleRateText(saved.ratePolicy.fixedRate)
          : std::string("Automatic");
  const auto fixedLiveRate =
      state.runtime.configuredRatePolicy.mode ==
              pipetune::SampleRateMode::fixed
          ? sampleRateText(
                state.runtime.configuredRatePolicy.fixedRate)
          : std::string("Automatic");

  auto sections = std::vector<StatusSection>{};
  sections.reserve(7);
  sections.push_back({
      .id = "system",
      .label = "System",
      .items =
          {
              textItem("system.connection", "PipeTune",
                       connectionText(state.connection), unavailable),
              textItem(
                  "system.default-sink", "Virtual sink active",
                  connected ? yesNo(state.runtime.defaultSinkActive) : "—",
                  connected && !state.runtime.defaultSinkActive
                      ? StatusSeverity::warning
                      : unavailable),
              textItem("system.pipewire-idle", "PipeWire idle",
                       connected ? yesNo(state.runtime.pipeWireIdle) : "—",
                       unavailable),
          },
  });
  sections.push_back({
      .id = "live-configuration",
      .label = "Live Configuration",
      .items =
          {
              textItem(
                  "live.processing", "Processing",
                  connected
                      ? processingModeText(state.runtime.processingMode)
                      : "—",
                  unavailable),
              textItem("live.preset", "Preset",
                       connected ? shortPath(livePreset) : "—",
                       unavailable, livePreset.string()),
              numericTextItem(
                  "live.plugins", "Active plugins",
                  connected
                      ? std::to_string(state.runtime.activePluginCount)
                      : "—",
                  connected
                      ? static_cast<double>(
                            state.runtime.activePluginCount)
                      : 0.0,
                  "plugins", 0.0,
                  connected
                      ? static_cast<double>(
                            state.runtime.activePluginCount)
                      : 0.0,
                  unavailable),
          },
  });
  sections.push_back({
      .id = "saved-configuration",
      .label = "Saved Configuration",
      .items =
          {
              textItem("saved.processing", "Processing",
                       configuredProcessingText(saved)),
              textItem("saved.preset", "Preset",
                       saved.presetFound ? shortPath(savedPreset) : "—",
                       StatusSeverity::normal, savedPreset.string()),
              textItem("saved.output", "Output", savedOutput,
                       StatusSeverity::normal, savedOutput),
              textItem("saved.rate-mode", "Rate mode",
                       sampleRateModeText(saved.ratePolicy.mode)),
              textItem("saved.fixed-rate", "Fixed rate", fixedSavedRate),
              textItem(
                  "saved.rate-enforcement", "Rate enforcement",
                  rateEnforcementText(saved.ratePolicy.enforcement)),
              textItem("saved.backend", "DSP backend",
                       backendText(saved.dspBackend)),
              textItem("saved.simd-variant", "SIMD variant",
                       simdVariantText(saved.dspSimdVariant)),
              textItem("saved.idle-policy", "Idle policy",
                       idlePolicyText(saved.dspIdlePolicy)),
          },
  });
  sections.push_back({
      .id = "routing",
      .label = "Routing",
      .items =
          {
              textItem("routing.preference", "Preference", livePreference,
                       unavailable, livePreference),
              outputItem(
                  "routing.selected-output", "Selected output",
                  state.runtime, state.runtime.selectedTarget,
                  state.runtime.selectedTarget.empty()
                      ? StatusSeverity::warning
                      : unavailable),
              textItem(
                  "routing.reason", "Selection reason",
                  connected
                      ? selectionReasonText(
                            state.runtime.outputSelectionReason)
                      : "—",
                  state.runtime.outputSelectionReason ==
                          pipetune::ControlOutputSelectionReason::fallback
                      ? StatusSeverity::warning
                      : unavailable),
          },
  });
  sections.push_back({
      .id = "input-rates",
      .label = "Input / Rates",
      .items =
          {
              textItem("input.format", "Input format",
                       connected &&
                               !state.runtime.inputSampleFormat.empty()
                           ? state.runtime.inputSampleFormat
                           : "—",
                       unavailable),
              textItem(
                  "input.sample-rate", "Input sample rate",
                  connected
                      ? sampleRateText(state.runtime.inputSampleRate)
                      : "—",
                  unavailable),
              textItem(
                  "input.channels", "Input channels",
                  connected && state.runtime.inputChannelCount != 0
                      ? std::to_string(state.runtime.inputChannelCount)
                      : "—",
                  unavailable),
              textItem("input.frame-rate", "Measured frame rate",
                       input.frameRate, unavailable),
              textItem("input.last-received", "Last input",
                       input.lastReceived, unavailable),
              textItem("input.pcm-rate", "PCM data rate",
                       input.pcmDataRate, unavailable),
              textItem(
                  "rates.mode", "Live rate mode",
                  connected
                      ? sampleRateModeText(
                            state.runtime.configuredRatePolicy.mode)
                      : "—",
                  unavailable),
              textItem("rates.fixed", "Live fixed rate",
                       connected ? fixedLiveRate : "—", unavailable),
              textItem(
                  "rates.enforcement", "Live enforcement",
                  connected
                      ? rateEnforcementText(
                            state.runtime.configuredRatePolicy.enforcement)
                      : "—",
                  unavailable),
              textItem(
                  "rates.dsp", "DSP rate",
                  connected ? sampleRateText(state.runtime.dspSampleRate)
                            : "—",
                  unavailable),
              textItem(
                  "rates.selected-output", "Selected output rate",
                  connected
                      ? sampleRateText(
                            state.runtime.selectedOutputSampleRate)
                      : "—",
                  unavailable),
              textItem(
                  "rates.active-output", "Active output rate",
                  connected
                      ? sampleRateText(
                            state.runtime.activeOutputSampleRate)
                      : "—",
                  unavailable),
          },
  });
  sections.push_back({
      .id = "dsp-performance",
      .label = "DSP / Performance",
      .items =
          {
              textItem(
                  "dsp.backend", "Configured backend",
                  connected
                      ? backendText(state.runtime.configuredDspBackend)
                      : "—",
                  unavailable),
              textItem(
                  "dsp.simd-variant", "Configured SIMD",
                  connected
                      ? simdVariantText(
                            state.runtime.configuredDspSimdVariant)
                      : "—",
                  unavailable),
              textItem(
                  "dsp.effective-variant", "Effective backend",
                  connected
                      ? effectiveVariantText(
                            state.runtime.effectiveDspVariant)
                      : "—",
                  state.runtime.dspBackendFallback
                      ? StatusSeverity::warning
                      : unavailable),
              textItem(
                  "dsp.idle-policy", "Idle policy",
                  connected
                      ? idlePolicyText(state.runtime.dspIdlePolicy)
                      : "—",
                  unavailable),
              textItem(
                  "dsp.idle-state", "Idle state",
                  connected
                      ? idleStateText(state.runtime.dspIdleState)
                      : "—",
                  unavailable),
              dspTimeItem(state),
              dspLoadItem(state),
              textItem(
                  "errors.overrun", "Overrun frames",
                  connected
                      ? std::to_string(state.runtime.overrunFrames)
                      : "—",
                  connected
                      ? nonzeroSeverity(state.runtime.overrunFrames)
                      : unavailable),
              textItem(
                  "errors.underrun", "Underrun frames",
                  connected
                      ? std::to_string(state.runtime.underrunFrames)
                      : "—",
                  connected
                      ? nonzeroSeverity(state.runtime.underrunFrames)
                      : unavailable),
              textItem(
                  "errors.processing", "Processing errors",
                  connected
                      ? std::to_string(state.runtime.processingErrors)
                      : "—",
                  connected
                      ? nonzeroSeverity(state.runtime.processingErrors)
                      : unavailable),
          },
  });
  sections.push_back({
      .id = "errors",
      .label = "Errors",
      .items =
          {
              textItem(
                  "errors.configuration", "Configuration",
                  connected
                      ? errorText(state.runtime.configurationError)
                      : "—",
                  connected
                      ? errorSeverity(
                            state.runtime.configurationError)
                      : unavailable,
                  state.runtime.configurationError),
              textItem(
                  "errors.rate", "Rate",
                  connected ? errorText(state.runtime.rateError) : "—",
                  connected ? errorSeverity(state.runtime.rateError)
                            : unavailable,
                  state.runtime.rateError),
              textItem(
                  "errors.backend", "DSP backend",
                  connected
                      ? errorText(state.runtime.dspBackendError)
                      : "—",
                  connected
                      ? errorSeverity(state.runtime.dspBackendError)
                      : unavailable,
                  state.runtime.dspBackendError),
              textItem(
                  "errors.control", "Control",
                  errorText(state.diagnostic),
                  errorSeverity(state.diagnostic), state.diagnostic),
              textItem(
                  "errors.warnings", "Preset warnings",
                  std::to_string(state.warnings.size()),
                  state.warnings.empty() ? StatusSeverity::normal
                                         : StatusSeverity::warning),
          },
  });
  return sections;
}

} // namespace pipetune_gtk
