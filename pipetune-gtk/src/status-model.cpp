#include "status-model.h"

#include "localization.h"

#include "pipetune/dsp_backend.h"

#include <algorithm>
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

static StatusItem numericLevelItem(
    std::string id, std::string label, std::string value,
    double numericValue, std::string unit, double minimum,
    double maximum, StatusSeverity severity, std::string tooltip) {
  auto item = numericTextItem(
      std::move(id), std::move(label), std::move(value), numericValue,
      std::move(unit), minimum, maximum, severity);
  item.displayKind = StatusDisplayKind::levelBar;
  item.tooltip = std::move(tooltip);
  return item;
}

static std::string fixedDecimal(double value, int precision) {
  auto stream = std::ostringstream{};
  stream << std::fixed << std::setprecision(precision) << value;
  return stream.str();
}

static std::string connectionText(ControlConnectionState connection) {
  switch (connection) {
  case ControlConnectionState::disconnected:
    return translate("Disconnected");
  case ControlConnectionState::connecting:
    return translate("Connecting");
  case ControlConnectionState::connected:
    return translate("Connected");
  }
  return translate("Unknown");
}

static std::string processingModeText(pipetune::ProcessingMode mode) {
  return mode == pipetune::ProcessingMode::preset ? translate("Preset")
                                                  : translate("Bypass");
}

static std::string configuredProcessingText(
    const pipetune::StartupConfig &config) {
  return config.presetFound ? translate("Preset") : translate("Bypass");
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
  return fixedDecimal(kilohertz, rate % 1000 == 0 ? 0 : 1) + " kHz";
}

static std::string sampleRateModeText(pipetune::SampleRateMode mode) {
  return mode == pipetune::SampleRateMode::maximum
             ? translate("Maximum")
             : translate("Fixed");
}

static std::string rateEnforcementText(
    pipetune::SampleRateEnforcement enforcement) {
  return enforcement == pipetune::SampleRateEnforcement::force
             ? translate("Force")
             : translate("Suggest");
}

static std::string backendText(pipetune::DspBackendKind backend) {
  return backend == pipetune::DspBackendKind::simd ? "SIMD"
                                                   : translate("Scalar");
}

static std::string simdVariantText(pipetune::DspSimdVariant variant) {
  const auto name = pipetune::dspSimdVariantName(variant);
  return name.empty() ? std::string(translate("Unknown"))
                      : std::string(name);
}

static std::string effectiveVariantText(
    const std::optional<pipetune::DspBackendVariant> &variant) {
  if (!variant.has_value()) {
    return translate("Unavailable");
  }
  const auto name = pipetune::dspBackendVariantName(*variant);
  return name.empty() ? std::string(translate("Unknown"))
                      : std::string(name);
}

static std::string filterStateText(pipetune::ControlFilterState state) {
  switch (state) {
  case pipetune::ControlFilterState::waiting:
    return translate("Waiting");
  case pipetune::ControlFilterState::active:
    return translate("Active");
  case pipetune::ControlFilterState::bypassed:
    return translate("Direct route");
  case pipetune::ControlFilterState::error:
    return translate("Error");
  }
  return translate("Unknown");
}

static std::string outputName(
    const pipetune::ControlFilterOutputStatus &output) {
  return output.targetDescription.empty() ? output.targetNodeName
                                          : output.targetDescription;
}

static std::string outputDetails(
    const pipetune::ControlRuntimeStatus &runtime) {
  if (runtime.filterOutputs.empty()) {
    return translate("No physical outputs");
  }
  auto details = std::string{};
  for (const auto &output : runtime.filterOutputs) {
    if (!details.empty()) {
      details += "\n";
    }
    details += outputName(output) + ": " + filterStateText(output.state);
    if (!output.error.empty()) {
      details += " — " + output.error;
    }
    if (outputName(output) != output.targetNodeName) {
      details += "\n  " + output.targetNodeName;
    }
  }
  return details;
}

static std::string outputRateDetails(
    const pipetune::ControlRuntimeStatus &runtime) {
  if (runtime.filterOutputs.empty()) {
    return translate("No physical outputs");
  }
  auto details = std::string{};
  for (const auto &output : runtime.filterOutputs) {
    if (!details.empty()) {
      details += "\n";
    }
    details += outputName(output) + ": DSP " +
               sampleRateText(output.dspSampleRate) + " → PipeWire " +
               sampleRateText(output.outputSampleRate) + " → Physical " +
               sampleRateText(output.activeOutputSampleRate);
    if (output.rateFallback) {
      details += " (" + std::string(translate("resampling")) + ")";
    }
  }
  return details;
}

static std::size_t countFilterState(
    const pipetune::ControlRuntimeStatus &runtime,
    pipetune::ControlFilterState state) {
  return static_cast<std::size_t>(std::count_if(
      runtime.filterOutputs.begin(), runtime.filterOutputs.end(),
      [state](const auto &output) { return output.state == state; }));
}

static StatusSeverity outputSeverity(
    const pipetune::ControlRuntimeStatus &runtime) {
  if (runtime.filterOutputs.empty()) {
    return StatusSeverity::warning;
  }
  const auto hasError = std::any_of(
      runtime.filterOutputs.begin(), runtime.filterOutputs.end(),
      [](const auto &output) {
        return output.state == pipetune::ControlFilterState::error;
      });
  if (hasError) {
    return StatusSeverity::error;
  }
  const auto allActive = std::all_of(
      runtime.filterOutputs.begin(), runtime.filterOutputs.end(),
      [](const auto &output) {
        return output.state == pipetune::ControlFilterState::active;
      });
  return allActive ? StatusSeverity::normal : StatusSeverity::warning;
}

static StatusItem dspLoadItem(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected ||
      !state.hasRuntimeStatus || !state.dspTiming.hasAverage ||
      !std::isfinite(state.dspTiming.loadPercent) ||
      state.dspTiming.loadPercent < 0.0) {
    return textItem("dsp.load", translate("Load"), "—");
  }
  const auto load = state.dspTiming.loadPercent;
  return numericLevelItem(
      "dsp.load", translate("Load"), fixedDecimal(load, 1) + "%", load,
      "%", 0.0, 100.0,
      load > 100.0 ? StatusSeverity::error : StatusSeverity::normal,
      translate("Aggregate DSP CPU time; graph capped at 100%."));
}

static StatusItem dspTimeItem(const ApplicationState &state) {
  if (state.connection != ControlConnectionState::connected ||
      !state.hasRuntimeStatus || !state.dspTiming.hasAverage ||
      !std::isfinite(state.dspTiming.nanosecondsPerFrame) ||
      state.dspTiming.nanosecondsPerFrame < 0.0) {
    return textItem("dsp.processing-time", translate("Processing time"),
                    "—");
  }
  const auto microseconds = state.dspTiming.nanosecondsPerFrame / 1000.0;
  return numericTextItem(
      "dsp.processing-time", translate("Processing time"),
      fixedDecimal(microseconds, 2) + " µs/frame", microseconds,
      "µs/frame", 0.0, microseconds);
}

static StatusSeverity nonzeroSeverity(std::uint64_t value) {
  return value == 0 ? StatusSeverity::normal : StatusSeverity::warning;
}

static std::string errorText(std::string_view error) {
  return error.empty() ? std::string(translate("None"))
                       : std::string(error);
}

static StatusSeverity errorSeverity(std::string_view error) {
  return error.empty() ? StatusSeverity::normal : StatusSeverity::error;
}

std::optional<StatusLevelPresentation>
statusLevelPresentation(const StatusItem &item) {
  if (item.displayKind != StatusDisplayKind::levelBar ||
      !item.numericValue.has_value() || !item.minimum.has_value() ||
      !item.maximum.has_value() || !std::isfinite(*item.numericValue) ||
      !std::isfinite(*item.minimum) || !std::isfinite(*item.maximum) ||
      *item.maximum <= *item.minimum) {
    return std::nullopt;
  }

  const auto value =
      std::clamp(*item.numericValue, *item.minimum, *item.maximum);
  const auto fraction =
      (value - *item.minimum) / (*item.maximum - *item.minimum);
  const auto hueStep =
      fraction >= 1.0
          ? std::uint8_t{10}
          : static_cast<std::uint8_t>(std::floor(fraction * 10.0));
  return StatusLevelPresentation{.clampedValue = value,
                                 .fraction = fraction,
                                 .hueStep = hueStep};
}

std::vector<StatusSection> buildStatusSections(
    const ApplicationState &state,
    const pipetune::StartupConfig &saved,
    std::uint64_t currentUnixMilliseconds) {
  static_cast<void>(currentUnixMilliseconds);
  const auto connected =
      state.connection == ControlConnectionState::connected &&
      state.hasRuntimeStatus;
  const auto unavailable = connected ? StatusSeverity::normal
                                     : StatusSeverity::warning;
  const auto livePreset = std::filesystem::path(state.runtime.activePreset);
  const auto savedPreset = saved.presetPath;
  const auto fixedSavedRate =
      saved.ratePolicy.mode == pipetune::SampleRateMode::fixed
          ? sampleRateText(saved.ratePolicy.fixedRate)
          : std::string(translate("Automatic"));
  const auto fixedLiveRate =
      state.runtime.configuredRatePolicy.mode ==
              pipetune::SampleRateMode::fixed
          ? sampleRateText(state.runtime.configuredRatePolicy.fixedRate)
          : std::string(translate("Automatic"));
  const auto details = connected ? outputDetails(state.runtime) : "—";
  const auto rates = connected ? outputRateDetails(state.runtime) : "—";
  const auto filters = connected ? outputSeverity(state.runtime)
                                 : StatusSeverity::warning;

  auto sections = std::vector<StatusSection>{};
  sections.reserve(7);
  sections.push_back({
      .id = "system",
      .label = translate("System"),
      .items =
          {textItem("system.connection", "PipeTune",
                    connectionText(state.connection), unavailable),
           textItem("system.policy-backend",
                    translate("WirePlumber policy"),
                    connected && !state.runtime.policyBackend.empty()
                        ? state.runtime.policyBackend
                        : "—",
                    unavailable),
           textItem("system.physical-outputs",
                    translate("Physical outputs"),
                    connected
                        ? std::to_string(state.runtime.filterOutputs.size())
                        : "—",
                    connected && state.runtime.filterOutputs.empty()
                        ? StatusSeverity::warning
                        : unavailable)},
  });
  sections.push_back({
      .id = "live-configuration",
      .label = translate("Live Configuration"),
      .items =
          {textItem("live.processing", translate("Processing"),
                    connected
                        ? processingModeText(state.runtime.processingMode)
                        : "—",
                    unavailable),
           textItem("live.preset", translate("Preset"),
                    connected ? shortPath(livePreset) : "—", unavailable,
                    livePreset.string()),
           numericTextItem(
               "live.plugins", translate("Active plugins"),
               connected ? std::to_string(state.runtime.activePluginCount)
                         : "—",
               connected
                   ? static_cast<double>(state.runtime.activePluginCount)
                   : 0.0,
               "plugins", 0.0,
               connected
                   ? static_cast<double>(state.runtime.activePluginCount)
                   : 0.0,
               unavailable)},
  });
  sections.push_back({
      .id = "saved-configuration",
      .label = translate("Saved Configuration"),
      .items =
          {textItem("saved.processing", translate("Processing"),
                    configuredProcessingText(saved)),
           textItem("saved.preset", translate("Preset"),
                    saved.presetFound ? shortPath(savedPreset) : "—",
                    StatusSeverity::normal, savedPreset.string()),
           textItem("saved.rate-mode", translate("Rate mode"),
                    sampleRateModeText(saved.ratePolicy.mode)),
           textItem("saved.fixed-rate", translate("Fixed rate"),
                    fixedSavedRate),
           textItem("saved.rate-enforcement",
                    translate("Rate enforcement"),
                    rateEnforcementText(saved.ratePolicy.enforcement)),
           textItem("saved.backend", translate("DSP backend"),
                    backendText(saved.dspBackend)),
           textItem("saved.simd-variant", translate("SIMD variant"),
                    simdVariantText(saved.dspSimdVariant))},
  });
  sections.push_back({
      .id = "outputs",
      .label = translate("Physical Output Filters"),
      .items =
          {textItem("outputs.active", translate("Filtered"),
                    connected
                        ? std::to_string(countFilterState(
                              state.runtime,
                              pipetune::ControlFilterState::active))
                        : "—",
                    filters),
           textItem("outputs.direct", translate("Direct routes"),
                    connected
                        ? std::to_string(countFilterState(
                              state.runtime,
                              pipetune::ControlFilterState::bypassed))
                        : "—",
                    filters),
           textItem("outputs.details", translate("Outputs"), details,
                    filters, details)},
  });
  sections.push_back({
      .id = "rates",
      .label = translate("Sample Rates"),
      .items =
          {textItem("rates.mode", translate("Live rate mode"),
                    connected
                        ? sampleRateModeText(
                              state.runtime.configuredRatePolicy.mode)
                        : "—",
                    unavailable),
           textItem("rates.fixed", translate("Live fixed rate"),
                    connected ? fixedLiveRate : "—", unavailable),
           textItem("rates.enforcement", translate("Live enforcement"),
                    connected
                        ? rateEnforcementText(
                              state.runtime.configuredRatePolicy.enforcement)
                        : "—",
                    unavailable),
           textItem("rates.outputs", translate("Per-output rates"), rates,
                    filters, rates)},
  });
  sections.push_back({
      .id = "dsp-performance",
      .label = translate("DSP / Performance"),
      .items =
          {textItem("dsp.backend", translate("Configured backend"),
                    connected
                        ? backendText(state.runtime.configuredDspBackend)
                        : "—",
                    unavailable),
           textItem("dsp.simd-variant", translate("Configured SIMD"),
                    connected
                        ? simdVariantText(
                              state.runtime.configuredDspSimdVariant)
                        : "—",
                    unavailable),
           textItem("dsp.effective-variant",
                    translate("Effective backend"),
                    connected
                        ? effectiveVariantText(
                              state.runtime.effectiveDspVariant)
                        : "—",
                    state.runtime.dspBackendFallback
                        ? StatusSeverity::warning
                        : unavailable),
           dspTimeItem(state), dspLoadItem(state),
           textItem("errors.overrun", translate("Overrun frames"),
                    connected
                        ? std::to_string(state.runtime.overrunFrames)
                        : "—",
                    connected
                        ? nonzeroSeverity(state.runtime.overrunFrames)
                        : unavailable),
           textItem("errors.underrun", translate("Underrun frames"),
                    connected
                        ? std::to_string(state.runtime.underrunFrames)
                        : "—",
                    connected
                        ? nonzeroSeverity(state.runtime.underrunFrames)
                        : unavailable),
           textItem("errors.processing", translate("Processing errors"),
                    connected
                        ? std::to_string(state.runtime.processingErrors)
                        : "—",
                    connected
                        ? nonzeroSeverity(state.runtime.processingErrors)
                        : unavailable)},
  });
  sections.push_back({
      .id = "errors",
      .label = translate("Errors"),
      .items =
          {textItem("errors.configuration", translate("Configuration"),
                    connected
                        ? errorText(state.runtime.configurationError)
                        : "—",
                    connected
                        ? errorSeverity(state.runtime.configurationError)
                        : unavailable,
                    state.runtime.configurationError),
           textItem("errors.outputs", translate("Output filters"), details,
                    filters, details),
           textItem("errors.backend", translate("DSP backend"),
                    connected ? errorText(state.runtime.dspBackendError)
                              : "—",
                    connected
                        ? errorSeverity(state.runtime.dspBackendError)
                        : unavailable,
                    state.runtime.dspBackendError),
           textItem("errors.control", translate("Control"),
                    errorText(state.diagnostic),
                    errorSeverity(state.diagnostic), state.diagnostic),
           textItem("errors.warnings", translate("Preset warnings"),
                    std::to_string(state.warnings.size()),
                    state.warnings.empty() ? StatusSeverity::normal
                                           : StatusSeverity::warning)},
  });
  return sections;
}

} // namespace pipetune_gtk
