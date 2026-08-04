#include "action-log.h"
#include "application-state.h"
#include "settings-transaction.h"
#include "status-model.h"

#include "pipetune/startup_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool approximately(double actual, double expected) {
  return std::abs(actual - expected) < 0.000001;
}

static pipetune::StartupConfig baseConfig() {
  return {
      .presetFound = true,
      .presetPath = "/tmp/base.effetune_preset",
      .ratePolicy = pipetune::defaultSampleRatePolicy(),
      .dspBackend = pipetune::DspBackendKind::scalar,
      .dspSimdVariant = pipetune::DspSimdVariant::automatic,
  };
}

static bool testLiveCoalescingApplyAndCancel() {
  const auto saved = baseConfig();
  auto transaction =
      pipetune_gtk::beginSettingsTransaction(saved, saved, true);
  auto desired = saved;
  desired.ratePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 96000,
      .enforcement = pipetune::SampleRateEnforcement::force,
  };
  desired.dspBackend = pipetune::DspBackendKind::simd;
  desired.dspSimdVariant = pipetune::DspSimdVariant::x86_64_v3;
  desired.presetPath = "/tmp/next.effetune_preset";
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::rate,
             "rate must be applied before backend and processing") ||
      !check(pipetune_gtk::beginSettingsOperation(
                 transaction, pipetune_gtk::SettingsOperation::rate),
             "the first rate operation must start")) {
    return false;
  }

  desired.ratePolicy.fixedRate = 192000;
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  auto firstConfirmation = saved;
  firstConfirmation.ratePolicy.fixedRate = 96000;
  firstConfirmation.ratePolicy.mode = pipetune::SampleRateMode::fixed;
  firstConfirmation.ratePolicy.enforcement =
      pipetune::SampleRateEnforcement::force;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, firstConfirmation, {});
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::rate,
             "an edit during a rate request must coalesce into a follow-up")) {
    return false;
  }

  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::rate);
  auto liveConfirmation = firstConfirmation;
  liveConfirmation.ratePolicy = desired.ratePolicy;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, liveConfirmation, {});
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::dspBackend,
             "backend must follow the confirmed rate")) {
    return false;
  }

  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  liveConfirmation.dspBackend = desired.dspBackend;
  liveConfirmation.dspSimdVariant = desired.dspSimdVariant;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, liveConfirmation, {});
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::processing,
             "processing must follow rate and backend")) {
    return false;
  }

  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::processing);
  liveConfirmation.presetPath = desired.presetPath;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, liveConfirmation, {});
  if (!check(pipetune_gtk::settingsTransactionCanApply(transaction) &&
                 pipetune_gtk::settingsTransactionIsDirty(transaction),
             "Apply must enable after every live change is confirmed")) {
    return false;
  }

  pipetune_gtk::completeSettingsPersistence(transaction, true, {});
  if (!check(!pipetune_gtk::settingsTransactionIsDirty(transaction),
             "successful Apply must establish a saved baseline") ||
      !check(!pipetune_gtk::settingsTransactionShouldClose(transaction),
             "Apply must leave the dialog open")) {
    return false;
  }

  desired = transaction.desiredLive;
  desired.dspBackend = pipetune::DspBackendKind::scalar;
  desired.dspSimdVariant = pipetune::DspSimdVariant::automatic;
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  liveConfirmation = transaction.confirmedLive;
  liveConfirmation.dspBackend = desired.dspBackend;
  liveConfirmation.dspSimdVariant = desired.dspSimdVariant;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, liveConfirmation, {});
  pipetune_gtk::requestSettingsCancel(transaction);
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::dspBackend,
             "Cancel must restore the captured live baseline")) {
    return false;
  }
  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  pipetune_gtk::completeSettingsOperation(
      transaction, true, transaction.baselineLive, {});
  return check(pipetune_gtk::settingsTransactionShouldClose(transaction),
               "Cancel may close only after rollback is confirmed");
}

static bool testFailuresDisconnectAndConflict() {
  const auto saved = baseConfig();
  auto transaction =
      pipetune_gtk::beginSettingsTransaction(saved, saved, true);
  auto desired = saved;
  desired.dspBackend = pipetune::DspBackendKind::simd;
  desired.dspSimdVariant = pipetune::DspSimdVariant::x86_64_v3;
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  pipetune_gtk::completeSettingsOperation(
      transaction, false, saved, "backend rejected");
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::none &&
                 transaction.diagnostic == "backend rejected" &&
                 !pipetune_gtk::settingsTransactionCanApply(transaction),
             "a rejected edit must stop and remain visible")) {
    return false;
  }

  pipetune_gtk::editSettingsTransaction(transaction, desired);
  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  pipetune_gtk::markSettingsDisconnected(transaction, "daemon unavailable");
  if (!check(!transaction.connected &&
                 pipetune_gtk::nextSettingsOperation(transaction) ==
                     pipetune_gtk::SettingsOperation::none,
             "disconnected settings must be read-only")) {
    return false;
  }

  pipetune_gtk::reconnectSettingsTransaction(transaction, saved);
  if (!check(transaction.connected &&
                 pipetune_gtk::nextSettingsOperation(transaction) ==
                     pipetune_gtk::SettingsOperation::dspBackend,
             "reconnection must resume the desired edit")) {
    return false;
  }

  transaction = pipetune_gtk::beginSettingsTransaction(saved, saved, true);
  auto external = saved;
  external.ratePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 48000,
      .enforcement = pipetune::SampleRateEnforcement::suggest,
  };
  pipetune_gtk::observeSettingsRuntime(transaction, external);
  return check(transaction.conflict &&
                   !pipetune_gtk::settingsTransactionCanApply(transaction),
               "an unexpected external change must cause a conflict");
}

static const pipetune_gtk::StatusSection *findSection(
    const std::vector<pipetune_gtk::StatusSection> &sections,
    std::string_view id) {
  const auto found = std::find_if(
      sections.begin(), sections.end(),
      [id](const auto &section) { return section.id == id; });
  return found == sections.end() ? nullptr : &*found;
}

static const pipetune_gtk::StatusItem *findItem(
    const pipetune_gtk::StatusSection &section, std::string_view id) {
  const auto found = std::find_if(
      section.items.begin(), section.items.end(),
      [id](const auto &item) { return item.id == id; });
  return found == section.items.end() ? nullptr : &*found;
}

static pipetune::ControlFilterOutputStatus activeFilter() {
  return {
      .targetNodeName =
          "alsa_output.usb-Very_Long_USB_Audio_Device_Name.analog-stereo",
      .targetDescription = "Studio DAC",
      .filterNodeName = "pipetune.filter.studio-dac",
      .state = pipetune::ControlFilterState::active,
      .error = {},
      .channelCount = 2,
      .sampleRateCapabilities = {},
      .dspSampleRate = 192000,
      .outputSampleRate = 96000,
      .activeOutputSampleRate = 96000,
      .rateFallback = true,
      .latencyFrames = 64,
      .overrunFrames = 1,
      .underrunFrames = 2,
      .processingErrors = 3,
      .dspProcessedFrames = 192000,
      .dspProcessingNanoseconds = 1000000,
  };
}

static bool testStructuredStatusModel() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime.processingMode = pipetune::ProcessingMode::preset;
  state.runtime.activePreset = "/tmp/live.effetune_preset";
  state.runtime.activePluginCount = 4;
  state.runtime.policyBackend = "wireplumber-0.5";
  state.runtime.filterOutputs = {activeFilter()};
  state.runtime.configuredRatePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force,
  };
  state.runtime.configuredDspBackend = pipetune::DspBackendKind::simd;
  state.runtime.configuredDspSimdVariant =
      pipetune::DspSimdVariant::x86_64_v3;
  state.runtime.effectiveDspBackend = pipetune::DspBackendKind::simd;
  state.runtime.effectiveDspVariant =
      pipetune::DspBackendVariant::x86_64_v3;
  state.runtime.overrunFrames = 1;
  state.runtime.underrunFrames = 2;
  state.runtime.processingErrors = 3;
  state.dspTiming.hasAverage = true;
  state.dspTiming.nanosecondsPerFrame = 1000.0;
  state.dspTiming.loadPercent = 20.0;

  const auto sections = pipetune_gtk::buildStatusSections(
      state, baseConfig(), 1704164645012ULL);
  const auto *performance = findSection(sections, "dsp-performance");
  const auto *outputs = findSection(sections, "outputs");
  const auto *rates = findSection(sections, "rates");
  if (!check(sections.size() == 7,
             "the status tree must expose seven stable sections") ||
      !check(performance != nullptr && outputs != nullptr && rates != nullptr,
             "required status sections are missing")) {
    return false;
  }
  const auto *load = findItem(*performance, "dsp.load");
  const auto *overrun = findItem(*performance, "errors.overrun");
  const auto *details = findItem(*outputs, "outputs.details");
  const auto *rateDetails = findItem(*rates, "rates.outputs");
  return check(load != nullptr && load->numericValue == 20.0 &&
                   load->unit == "%" && load->minimum == 0.0 &&
                   load->maximum == 100.0 &&
                   load->displayKind ==
                       pipetune_gtk::StatusDisplayKind::levelBar,
               "DSP load must request a bounded graph") &&
         check(overrun != nullptr && overrun->value == "1",
               "runtime counters must remain separate") &&
         check(details != nullptr &&
                   details->value.find("Studio DAC: Active") !=
                       std::string::npos &&
                   details->tooltip.find(
                       "alsa_output.usb-Very_Long_USB_Audio_Device_Name") !=
                       std::string::npos,
               "filter details must include label, state, and node name") &&
         check(rateDetails != nullptr &&
                   rateDetails->value.find("DSP 192 kHz") !=
                       std::string::npos &&
                   rateDetails->value.find("PipeWire 96 kHz") !=
                       std::string::npos,
               "per-output rates must remain visible");
}

static bool testStatusLevelPresentation() {
  auto item = pipetune_gtk::StatusItem{
      .id = "dsp.load",
      .label = "Load",
      .value = "0.0%",
      .numericValue = 0.0,
      .unit = "%",
      .severity = pipetune_gtk::StatusSeverity::normal,
      .displayKind = pipetune_gtk::StatusDisplayKind::levelBar,
      .minimum = 0.0,
      .maximum = 100.0,
      .tooltip = {},
  };
  struct LevelCase {
    double value;
    double clamped;
    double fraction;
    std::uint8_t hueStep;
  };
  constexpr auto cases = std::array{
      LevelCase{.value = 0.0, .clamped = 0.0, .fraction = 0.0,
                .hueStep = 0},
      LevelCase{.value = 10.0, .clamped = 10.0, .fraction = 0.1,
                .hueStep = 1},
      LevelCase{.value = 99.9, .clamped = 99.9, .fraction = 0.999,
                .hueStep = 9},
      LevelCase{.value = 120.0, .clamped = 100.0, .fraction = 1.0,
                .hueStep = 10},
  };
  for (const auto &expected : cases) {
    item.numericValue = expected.value;
    const auto presentation =
        pipetune_gtk::statusLevelPresentation(item);
    if (!check(presentation.has_value() &&
                   approximately(presentation->clampedValue,
                                 expected.clamped) &&
                   approximately(presentation->fraction,
                                 expected.fraction) &&
                   presentation->hueStep == expected.hueStep,
               "bounded status level presentation differs")) {
      return false;
    }
  }
  item.numericValue = std::numeric_limits<double>::infinity();
  return check(!pipetune_gtk::statusLevelPresentation(item).has_value(),
               "non-finite status levels must not be graphed");
}

static bool testActionLogHistory() {
  auto log = pipetune_gtk::createActionLog(500);
  const auto pending = pipetune_gtk::appendPendingAction(
      log, 1000, pipetune_gtk::ActionLogCategory::settings,
      pipetune_gtk::localizedMessage("Change rate", {}),
      pipetune_gtk::technicalMessage("192 kHz"));
  pipetune_gtk::completePendingAction(
      log, pending, 1010, true, pipetune_gtk::ActionLogSeverity::info,
      pipetune_gtk::localizedMessage("Rate changed", {}),
      pipetune_gtk::technicalMessage({}));
  if (!check(log.entries.size() == 1 &&
                 log.entries.front().state ==
                     pipetune_gtk::ActionLogState::success,
             "pending actions must be completed in place")) {
    return false;
  }

  pipetune_gtk::appendAction(
      log, 1020, pipetune_gtk::ActionLogSeverity::warning,
      pipetune_gtk::ActionLogCategory::control,
      pipetune_gtk::ActionLogState::failure,
      pipetune_gtk::localizedMessage("Filter bypassed", {}),
      pipetune_gtk::localizedMessage("Output failed open", {}));
  pipetune_gtk::appendAction(
      log, 1030, pipetune_gtk::ActionLogSeverity::error,
      pipetune_gtk::ActionLogCategory::persistence,
      pipetune_gtk::ActionLogState::failure,
      pipetune_gtk::localizedMessage("Apply failed", {}),
      pipetune_gtk::technicalMessage("Read-only file system"));
  const auto warnings = pipetune_gtk::filteredActionLogEntries(
      log, pipetune_gtk::ActionLogFilter::warnings);
  const auto errors = pipetune_gtk::filteredActionLogEntries(
      log, pipetune_gtk::ActionLogFilter::errors);
  if (!check(warnings.size() == 2 && errors.size() == 1,
             "log severity filters must retain history by threshold")) {
    return false;
  }

  for (auto index = std::uint64_t{0}; index < 502; ++index) {
    pipetune_gtk::appendAction(
        log, 2000 + index, pipetune_gtk::ActionLogSeverity::info,
        pipetune_gtk::ActionLogCategory::control,
        pipetune_gtk::ActionLogState::success,
        pipetune_gtk::localizedMessage("Status", {}),
        pipetune_gtk::technicalMessage({}));
  }
  if (!check(log.entries.size() == 500 &&
                 log.entries.front().timestampUnixMilliseconds == 2002,
             "the log must retain only the newest 500 entries")) {
    return false;
  }
  pipetune_gtk::clearActionLog(log);
  return check(log.entries.empty(), "Clear must remove log history");
}

int main() {
  return testLiveCoalescingApplyAndCancel() &&
                 testFailuresDisconnectAndConflict() &&
                 testStructuredStatusModel() &&
                 testStatusLevelPresentation() && testActionLogHistory()
             ? 0
             : 1;
}
