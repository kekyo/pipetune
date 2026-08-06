#include "action-log.h"
#include "application-state.h"
#include "settings-transaction.h"
#include "status-model.h"

#include "pipetune/startup_config.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <string>
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
      .preferredOutputFound = true,
      .preferredOutput = "alsa_output.base",
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
  desired.preferredOutput = "alsa_output.first";
  desired.ratePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force,
  };
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::output,
             "output must be applied before dependent settings") ||
      !check(pipetune_gtk::beginSettingsOperation(
                 transaction, pipetune_gtk::SettingsOperation::output),
             "the first output operation must start")) {
    return false;
  }

  desired.preferredOutput = "alsa_output.coalesced";
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  auto firstConfirmation = saved;
  firstConfirmation.preferredOutput = "alsa_output.first";
  pipetune_gtk::completeSettingsOperation(
      transaction, true, firstConfirmation, {});
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::output,
             "an edit during a request must coalesce into one follow-up")) {
    return false;
  }

  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::output);
  auto outputConfirmation = firstConfirmation;
  outputConfirmation.preferredOutput = "alsa_output.coalesced";
  pipetune_gtk::completeSettingsOperation(
      transaction, true, outputConfirmation, {});
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::rate,
             "rate must follow the confirmed output")) {
    return false;
  }

  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::rate);
  auto liveConfirmation = outputConfirmation;
  liveConfirmation.ratePolicy = desired.ratePolicy;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, liveConfirmation, {});
  if (!check(pipetune_gtk::settingsTransactionCanApply(transaction),
             "Apply must enable after every live change is confirmed") ||
      !check(pipetune_gtk::settingsTransactionIsDirty(transaction),
             "confirmed live changes must remain unpersisted")) {
    return false;
  }

  pipetune_gtk::completeSettingsPersistence(transaction, true, {});
  if (!check(!pipetune_gtk::settingsTransactionIsDirty(transaction),
             "successful Apply must establish a new saved baseline") ||
      !check(!pipetune_gtk::settingsTransactionShouldClose(transaction),
             "Apply must leave the dialog open")) {
    return false;
  }

  desired = transaction.desiredLive;
  desired.dspBackend = pipetune::DspBackendKind::simd;
  desired.dspSimdVariant = pipetune::DspSimdVariant::x86_64_v3;
  pipetune_gtk::editSettingsTransaction(transaction, desired);
  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  liveConfirmation = transaction.confirmedLive;
  liveConfirmation.dspBackend = pipetune::DspBackendKind::simd;
  liveConfirmation.dspSimdVariant =
      pipetune::DspSimdVariant::x86_64_v3;
  pipetune_gtk::completeSettingsOperation(
      transaction, true, liveConfirmation, {});
  pipetune_gtk::requestSettingsCancel(transaction);
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::dspBackend,
             "Cancel must restore the live baseline")) {
    return false;
  }
  pipetune_gtk::beginSettingsOperation(
      transaction, pipetune_gtk::SettingsOperation::dspBackend);
  pipetune_gtk::completeSettingsOperation(
      transaction, true, transaction.baselineLive, {});
  return check(
      pipetune_gtk::settingsTransactionShouldClose(transaction),
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
                 pipetune_gtk::SettingsOperation::none,
             "a rejected live edit must not retry indefinitely") ||
      !check(transaction.diagnostic == "backend rejected",
             "a live rejection must remain visible") ||
      !check(!pipetune_gtk::settingsTransactionCanApply(transaction),
             "an unconfirmed live edit must not be persisted")) {
    return false;
  }

  pipetune_gtk::editSettingsTransaction(transaction, desired);
  if (!check(pipetune_gtk::nextSettingsOperation(transaction) ==
                 pipetune_gtk::SettingsOperation::dspBackend,
             "a subsequent user edit must allow retry")) {
    return false;
  }
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
             "reconnection must re-confirm and reapply the desired state")) {
    return false;
  }

  transaction = pipetune_gtk::beginSettingsTransaction(saved, saved, true);
  auto external = saved;
  external.preferredOutput = "alsa_output.external";
  pipetune_gtk::observeSettingsRuntime(transaction, external);
  return check(transaction.conflict,
               "an unexpected external change must be detected") &&
         check(!pipetune_gtk::settingsTransactionCanApply(transaction),
               "Apply must remain disabled after an external conflict");
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

static bool testStructuredStatusModel() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime.processingMode = pipetune::ProcessingMode::preset;
  state.runtime.activePreset = "/tmp/live.effetune_preset";
  state.runtime.activePluginCount = 4;
  state.runtime.preferredTarget =
      "alsa_output.usb-Very_Long_USB_Audio_Device_Name.analog-stereo";
  state.runtime.selectedTarget = state.runtime.preferredTarget;
  state.runtime.outputSelectionReason =
      pipetune::ControlOutputSelectionReason::preferred;
  state.runtime.availableOutputs = {{
      .name = state.runtime.preferredTarget,
      .description = "Studio DAC",
      .systemDefault = true,
      .preferred = true,
      .selected = true,
  }};
  state.runtime.defaultSinkActive = true;
  state.runtime.inputSampleFormat = "F32P";
  state.runtime.inputSampleRate = 200000;
  state.runtime.inputChannelCount = 2;
  state.runtime.configuredRatePolicy = {
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force,
  };
  state.runtime.dspSampleRate = 192000;
  state.runtime.graphSampleRate = 192000;
  state.runtime.configuredDspBackend = pipetune::DspBackendKind::simd;
  state.runtime.configuredDspSimdVariant =
      pipetune::DspSimdVariant::x86_64_v3;
  state.runtime.effectiveDspVariant =
      pipetune::DspBackendVariant::x86_64_v3;
  state.runtime.overrunFrames = 1;
  state.runtime.underrunFrames = 2;
  state.runtime.processingErrors = 3;
  state.dspTiming.hasAverage = true;
  state.dspTiming.nanosecondsPerFrame = 1000.0;
  const auto sections = pipetune_gtk::buildStatusSections(
      state, baseConfig(), 1704164645012ULL);
  const auto *performance = findSection(sections, "dsp-performance");
  const auto *routing = findSection(sections, "routing");
  if (!check(sections.size() == 7,
             "the status tree must expose seven stable sections") ||
      !check(performance != nullptr && routing != nullptr,
             "required status sections are missing")) {
    return false;
  }
  const auto *load = findItem(*performance, "dsp.load");
  const auto *overrun = findItem(*performance, "errors.overrun");
  const auto *underrun = findItem(*performance, "errors.underrun");
  const auto *processing =
      findItem(*performance, "errors.processing");
  const auto *processingTime =
      findItem(*performance, "dsp.processing-time");
  const auto *selected = findItem(*routing, "routing.selected-output");
  if (!check(load != nullptr && load->numericValue.has_value() &&
                 *load->numericValue == 20.0 && load->unit == "%" &&
                 load->minimum == 0.0 && load->maximum == 100.0 &&
                 load->displayKind ==
                     pipetune_gtk::StatusDisplayKind::levelBar,
             "DSP load must request a bounded graph presentation") ||
      !check(processingTime != nullptr &&
                 processingTime->displayKind ==
                     pipetune_gtk::StatusDisplayKind::text,
             "other numeric status rows must remain text") ||
      !check(overrun != nullptr && overrun->value == "1" &&
                 underrun != nullptr && underrun->value == "2" &&
                 processing != nullptr && processing->value == "3",
             "runtime counters must be separate status rows") ||
      !check(selected != nullptr && selected->value == "Studio DAC" &&
                 selected->tooltip.find(state.runtime.preferredTarget) !=
                     std::string::npos,
             "output status must show a short label with the full name "
             "available")) {
    return false;
  }
  for (const auto &section : sections) {
    for (const auto &item : section.items) {
      if (!check(item.value.find("•") == std::string::npos,
                 "status values must not pack fields with bullets")) {
        return false;
      }
    }
  }
  return true;
}

static bool testStatusLevelPresentation() {
  auto state = pipetune_gtk::initialApplicationState();
  state.connection = pipetune_gtk::ControlConnectionState::connected;
  state.hasRuntimeStatus = true;
  state.runtime.inputSampleRate = 200000;
  state.dspTiming.hasAverage = true;
  state.dspTiming.nanosecondsPerFrame = 1000.0;
  const auto sections = pipetune_gtk::buildStatusSections(
      state, baseConfig(), 1704164645012ULL);
  const auto *performance = findSection(sections, "dsp-performance");
  if (!check(performance != nullptr,
             "DSP performance section is unavailable")) {
    return false;
  }
  const auto *load = findItem(*performance, "dsp.load");
  if (!check(load != nullptr, "DSP load item is unavailable")) {
    return false;
  }

  struct LevelCase {
    double value;
    double clamped;
    double fraction;
    std::uint8_t hueStep;
  };
  constexpr auto cases = std::array{
      LevelCase{.value = 0.0, .clamped = 0.0, .fraction = 0.0,
                .hueStep = 0},
      LevelCase{.value = 9.99, .clamped = 9.99, .fraction = 0.0999,
                .hueStep = 0},
      LevelCase{.value = 10.0, .clamped = 10.0, .fraction = 0.1,
                .hueStep = 1},
      LevelCase{.value = 99.9, .clamped = 99.9, .fraction = 0.999,
                .hueStep = 9},
      LevelCase{.value = 100.0, .clamped = 100.0, .fraction = 1.0,
                .hueStep = 10},
      LevelCase{.value = 120.0, .clamped = 100.0, .fraction = 1.0,
                .hueStep = 10},
  };
  for (const auto &expected : cases) {
    auto item = *load;
    item.numericValue = expected.value;
    const auto presentation =
        pipetune_gtk::statusLevelPresentation(item);
    if (!check(
            presentation.has_value() &&
                approximately(presentation->clampedValue,
                              expected.clamped) &&
                approximately(presentation->fraction,
                              expected.fraction) &&
                presentation->hueStep == expected.hueStep,
            "bounded status level presentation differs")) {
      return false;
    }
  }

  auto invalid = *load;
  invalid.numericValue = std::numeric_limits<double>::infinity();
  if (!check(!pipetune_gtk::statusLevelPresentation(invalid).has_value(),
             "non-finite status levels must not be graphed")) {
    return false;
  }

  state.dspTiming.nanosecondsPerFrame = 6000.0;
  const auto overloadedSections = pipetune_gtk::buildStatusSections(
      state, baseConfig(), 1704164645012ULL);
  const auto *overloadedPerformance =
      findSection(overloadedSections, "dsp-performance");
  const auto *overloaded = overloadedPerformance == nullptr
                               ? nullptr
                               : findItem(*overloadedPerformance, "dsp.load");
  const auto overloadedPresentation =
      overloaded == nullptr
          ? std::optional<pipetune_gtk::StatusLevelPresentation>{}
          : pipetune_gtk::statusLevelPresentation(*overloaded);
  if (!check(overloaded != nullptr && overloaded->value == "120.0%" &&
                 overloaded->severity ==
                     pipetune_gtk::StatusSeverity::error &&
                 overloadedPresentation.has_value() &&
                 overloadedPresentation->clampedValue == 100.0 &&
                 overloadedPresentation->hueStep == 10,
             "overload text must remain actual while its graph is capped")) {
    return false;
  }

  state.dspTiming.hasAverage = false;
  const auto unavailableSections = pipetune_gtk::buildStatusSections(
      state, baseConfig(), 1704164645012ULL);
  const auto *unavailablePerformance =
      findSection(unavailableSections, "dsp-performance");
  const auto *unavailable =
      unavailablePerformance == nullptr
          ? nullptr
          : findItem(*unavailablePerformance, "dsp.load");
  return check(
      unavailable != nullptr && unavailable->value == "—" &&
          unavailable->displayKind ==
              pipetune_gtk::StatusDisplayKind::text &&
          !pipetune_gtk::statusLevelPresentation(*unavailable).has_value(),
      "unavailable DSP load must remain a text fallback");
}

static bool testActionLogHistory() {
  auto log = pipetune_gtk::createActionLog(500);
  const auto pending = pipetune_gtk::appendPendingAction(
      log, 1000, pipetune_gtk::ActionLogCategory::settings,
      pipetune_gtk::localizedMessage("Change output", {}),
      pipetune_gtk::technicalMessage("Studio DAC"));
  pipetune_gtk::completePendingAction(
      log, pending, 1010, true, pipetune_gtk::ActionLogSeverity::info,
      pipetune_gtk::localizedMessage("Output changed", {}),
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
      pipetune_gtk::localizedMessage("Fallback selected", {}),
      pipetune_gtk::localizedMessage(
          "Preferred output is unavailable", {}));
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
  return check(log.entries.empty(), "Clear must remove visible log history");
}

int main() {
  return testLiveCoalescingApplyAndCancel() &&
                 testFailuresDisconnectAndConflict() &&
                 testStructuredStatusModel() &&
                 testStatusLevelPresentation() && testActionLogHistory()
             ? 0
             : 1;
}
