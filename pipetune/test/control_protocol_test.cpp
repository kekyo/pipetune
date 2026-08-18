/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/control_protocol.h"

#include <yyjson.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool replaceOnce(std::string &value, std::string_view from,
                        std::string_view to) {
  const auto position = value.find(from);
  if (position == std::string::npos) {
    return false;
  }
  value.replace(position, from.size(), to);
  return true;
}

static bool testRequests() {
  const auto statusJson = pipetune::makeStatusControlRequest();
  const auto status = pipetune::parseControlRequest(statusJson);
  if (!check(status.error.empty(), status.error) ||
      !check(status.request.command == pipetune::ControlCommand::status,
             "status request command differs") ||
      !check(status.request.presetPath.empty(),
             "status request must not contain a preset")) {
    return false;
  }

  const auto subscribeJson = pipetune::makeSubscribeControlRequest();
  const auto subscribe = pipetune::parseControlRequest(subscribeJson);
  if (!check(subscribe.error.empty(), subscribe.error) ||
      !check(subscribe.request.command ==
                 pipetune::ControlCommand::subscribe,
             "subscribe request command differs") ||
      !check(subscribe.request.presetPath.empty(),
             "subscribe request must not contain a preset")) {
    return false;
  }

  const auto bypassJson = pipetune::makeBypassControlRequest();
  const auto bypass = pipetune::parseControlRequest(bypassJson);
  if (!check(bypass.error.empty(), bypass.error) ||
      !check(bypass.request.command == pipetune::ControlCommand::bypass,
             "bypass request command differs") ||
      !check(bypass.request.presetPath.empty(),
             "bypass request must not contain a preset")) {
    return false;
  }

  const auto loadJson = pipetune::makeLoadPresetControlRequest(
      "/tmp/music \"wide\".effetune_preset");
  const auto load = pipetune::parseControlRequest(loadJson);
  if (!check(load.error.empty(), load.error) ||
      !check(load.request.command == pipetune::ControlCommand::loadPreset,
             "load request command differs") ||
      !check(load.request.presetPath ==
                 "/tmp/music \"wide\".effetune_preset",
             "load request preset differs")) {
    return false;
  }

  const auto setAutomaticRate = pipetune::parseControlRequest(
      pipetune::makeSetRateControlRequest(
          {.mode = pipetune::SampleRateMode::automatic,
           .fixedRate = 0,
           .enforcement = pipetune::SampleRateEnforcement::suggest}));
  const auto setFixedRate = pipetune::parseControlRequest(
      pipetune::makeSetRateControlRequest(
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 192000,
           .enforcement = pipetune::SampleRateEnforcement::force}));
  const auto setDspBackend = pipetune::parseControlRequest(
      pipetune::makeSetDspBackendControlRequest(
          pipetune::DspBackendKind::simd,
          pipetune::DspSimdVariant::x86_64_v3));
  const auto setDspIdleIgnore = pipetune::parseControlRequest(
      pipetune::makeSetDspIdleControlRequest(
          {.timeoutMilliseconds = 0}));
  const auto setDspIdleMinimum = pipetune::parseControlRequest(
      pipetune::makeSetDspIdleControlRequest(
          {.timeoutMilliseconds = 100}));
  const auto setDspIdleMaximum = pipetune::parseControlRequest(
      pipetune::makeSetDspIdleControlRequest(
          {.timeoutMilliseconds = 5000}));
  return check(setAutomaticRate.error.empty(), setAutomaticRate.error) &&
         check(setAutomaticRate.request.command ==
                       pipetune::ControlCommand::setRate &&
                   setAutomaticRate.request.ratePolicy.mode ==
                       pipetune::SampleRateMode::automatic &&
                   setAutomaticRate.request.ratePolicy.fixedRate == 0 &&
                   setAutomaticRate.request.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::suggest,
               "set-rate automatic request differs") &&
         check(setFixedRate.error.empty(), setFixedRate.error) &&
         check(setFixedRate.request.command ==
                       pipetune::ControlCommand::setRate &&
                   setFixedRate.request.ratePolicy.mode ==
                       pipetune::SampleRateMode::fixed &&
                   setFixedRate.request.ratePolicy.fixedRate == 192000 &&
                   setFixedRate.request.ratePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::force,
               "set-rate fixed request differs") &&
         check(setDspBackend.error.empty(), setDspBackend.error) &&
         check(setDspBackend.request.command ==
                       pipetune::ControlCommand::setDspBackend &&
                   setDspBackend.request.dspBackend ==
                       pipetune::DspBackendKind::simd &&
                   setDspBackend.request.dspSimdVariant ==
                       pipetune::DspSimdVariant::x86_64_v3,
               "set-dsp-backend request differs") &&
         check(setDspIdleIgnore.error.empty(),
               setDspIdleIgnore.error) &&
         check(setDspIdleIgnore.request.command ==
                       pipetune::ControlCommand::setDspIdle &&
                   setDspIdleIgnore.request.dspIdlePolicy.timeoutMilliseconds ==
                       0,
               "set-dsp-idle ignore request differs") &&
         check(setDspIdleMinimum.error.empty(),
               setDspIdleMinimum.error) &&
         check(setDspIdleMinimum.request.command ==
                       pipetune::ControlCommand::setDspIdle &&
                   setDspIdleMinimum.request.dspIdlePolicy.timeoutMilliseconds ==
                       100,
               "set-dsp-idle minimum request differs") &&
         check(setDspIdleMaximum.error.empty(),
               setDspIdleMaximum.error) &&
         check(setDspIdleMaximum.request.command ==
                       pipetune::ControlCommand::setDspIdle &&
                   setDspIdleMaximum.request.dspIdlePolicy.timeoutMilliseconds ==
                       5000,
               "set-dsp-idle maximum request differs");
}

static bool testRejectedRequests() {
  constexpr auto inputs = std::array<std::string_view, 28>{
      "",
      "[]",
      R"json({"command":"unknown"})json",
      R"json({"command":"status","preset":"unexpected"})json",
      R"json({"command":"bypass","preset":"unexpected"})json",
      R"json({"command":"load"})json",
      R"json({"command":"load","preset":42})json",
      R"json({"command":"load","preset":""})json",
      R"json({"command":"set-rate"})json",
      R"json({"command":"set-rate","rateMode":"automatic","sampleRate":null,"enforcement":"suggest","extra":true})json",
      R"json({"command":"set-rate","rateMode":"max","sampleRate":null,"enforcement":"suggest"})json",
      R"json({"command":"set-rate","rateMode":"automatic","sampleRate":48000,"enforcement":"suggest"})json",
      R"json({"command":"set-rate","rateMode":"fixed","sampleRate":null,"enforcement":"suggest"})json",
      R"json({"command":"set-rate","rateMode":"fixed","sampleRate":88200,"enforcement":"suggest"})json",
      R"json({"command":"set-rate","rateMode":"fixed","sampleRate":48000,"enforcement":"strict"})json",
      R"json({"command":"set-rate","rateMode":"fixed","sampleRate":"48000","enforcement":"suggest"})json",
      R"json({"command":"set-dsp-backend"})json",
      R"json({"command":"set-dsp-backend","backend":"avx2"})json",
      R"json({"command":"set-dsp-backend","backend":"simd","extra":true})json",
      R"json({"command":"set-dsp-backend","backend":"simd","simdVariant":"avx2"})json",
      R"json({"command":"set-dsp-backend","backend":"scalar","simdVariant":"baseline"})json",
      R"json({"command":"set-dsp-backend","backend":"simd","simdVariant":42})json",
      R"json({"command":"set-dsp-idle"})json",
      R"json({"command":"set-dsp-idle","timeoutMilliseconds":0})json",
      R"json({"command":"set-dsp-idle","timeoutMilliseconds":99})json",
      R"json({"command":"set-dsp-idle","timeoutMilliseconds":150})json",
      R"json({"command":"set-dsp-idle","timeoutMilliseconds":5100})json",
      R"json({"command":"set-dsp-idle","timeoutMilliseconds":"100"})json"};
  for (const auto input : inputs) {
    if (!check(!pipetune::parseControlRequest(input).error.empty(),
               "invalid control request must be rejected")) {
      return false;
    }
  }
  return true;
}

static bool testSuccessResponse() {
  const auto warnings = std::array<pipetune::ControlWarning, 1>{
      pipetune::ControlWarning{.nodeIndex = 3,
                               .pluginName = "Future DSP",
                               .reason = "not available"}};
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::preset,
       .dspActivity = pipetune::DspActivity::draining,
       .dspIdlePolicy = {.timeoutMilliseconds = 2500},
       .activePreset = "/tmp/live.effetune_preset",
       .configurationError = {},
       .configurationRevision = 41,
       .activePluginCount = 7,
       .dspLatencyFrames = 64,
       .overrunFrames = 11,
       .underrunFrames = 12,
       .processingErrors = 13,
       .dspProcessedFrames = 96000,
       .dspProcessingNanoseconds = 240000000,
       .inputSampleFormat = "F32P",
       .inputSampleRate = 48000,
       .inputChannelCount = 2,
       .inputFramesReceived = 96000,
       .inputLastReceivedUnixMilliseconds = 1720000000123,
       .configuredRatePolicy =
           {.mode = pipetune::SampleRateMode::fixed,
            .fixedRate = 96000,
            .enforcement = pipetune::SampleRateEnforcement::force},
       .dspSampleRate = 96000,
       .graphSampleRate = 96000,
       .rateTransitioning = false,
       .rateError = {},
       .configuredDspBackend = pipetune::DspBackendKind::simd,
       .configuredDspSimdVariant =
           pipetune::DspSimdVariant::x86_64_v3,
       .effectiveDspBackend = pipetune::DspBackendKind::simd,
       .effectiveDspVariant =
           pipetune::DspBackendVariant::x86_64_v3,
       .dspBackendFallback = false,
       .dspBackendError = {},
       .availableDspBackends =
           {{
               {.kind = pipetune::DspBackendKind::scalar,
                .available = true,
                .cpuRequirement = "none",
                .error = {}},
               {.kind = pipetune::DspBackendKind::simd,
                .available = true,
                .cpuRequirement =
                    "x86-64 SSE2 architectural baseline",
                .error = {}},
           }},
       .availableDspVariants =
           {{.variant = pipetune::DspBackendVariant::scalar,
             .available = true,
             .cpuSupported = true,
             .cpuRequirement = "none",
             .error = {}},
            {.variant = pipetune::DspBackendVariant::simdBaseline,
             .available = true,
             .cpuSupported = true,
             .cpuRequirement =
                 "x86-64 SSE2 architectural baseline",
             .error = {}},
            {.variant = pipetune::DspBackendVariant::x86_64_v3,
             .available = true,
             .cpuSupported = true,
             .cpuRequirement = "x86-64-v3",
             .error = {}}}},
      warnings);
  const auto inspection = pipetune::inspectControlResponse(response);
  const auto parsed = pipetune::parseControlResponse(response);
  auto mismatchedPinnedVariant = response;
  if (!check(replaceOnce(
                 mismatchedPinnedVariant,
                 R"json("effectiveDspVariant":"x86-64-v3")json",
                 R"json("effectiveDspVariant":"baseline")json"),
             "cannot prepare mismatched pinned DSP variant")) {
    return false;
  }
  if (!check(inspection.valid, inspection.error) ||
      !check(inspection.success, "success response must report success") ||
      !check(parsed.valid, parsed.error) ||
      !check(parsed.success, "parsed response must report success") ||
      !check(parsed.kind == pipetune::ControlResponseKind::response,
             "ordinary response kind differs") ||
      !check(parsed.status.processingMode ==
                 pipetune::ProcessingMode::preset,
             "parsed response processing mode differs") ||
      !check(parsed.status.dspActivity ==
                     pipetune::DspActivity::draining &&
                 parsed.status.dspIdlePolicy.timeoutMilliseconds == 2500,
             "parsed response DSP idle state differs") ||
      !check(parsed.status.activePreset ==
                 "/tmp/live.effetune_preset",
             "parsed response preset differs") ||
      !check(parsed.status.configurationRevision == 41,
             "parsed response configuration revision differs") ||
      !check(parsed.status.activePluginCount == 7,
             "parsed response plugin count differs") ||
      !check(parsed.status.dspLatencyFrames == 64,
             "parsed response DSP latency differs") ||
      !check(parsed.status.overrunFrames == 11 &&
                 parsed.status.underrunFrames == 12 &&
                 parsed.status.processingErrors == 13,
             "parsed response counters differ") ||
      !check(parsed.status.dspProcessedFrames == 96000 &&
                 parsed.status.dspProcessingNanoseconds == 240000000,
             "parsed response DSP performance counters differ") ||
      !check(parsed.status.inputSampleFormat == "F32P" &&
                 parsed.status.inputSampleRate == 48000 &&
                 parsed.status.inputChannelCount == 2,
             "parsed response input format differs") ||
      !check(parsed.status.inputFramesReceived == 96000 &&
                 parsed.status.inputLastReceivedUnixMilliseconds ==
                     1720000000123ULL,
             "parsed response input telemetry differs") ||
      !check(parsed.status.configuredRatePolicy.mode ==
                     pipetune::SampleRateMode::fixed &&
                 parsed.status.configuredRatePolicy.fixedRate == 96000 &&
                 parsed.status.configuredRatePolicy.enforcement ==
                     pipetune::SampleRateEnforcement::force &&
                 parsed.status.dspSampleRate == 96000 &&
                 parsed.status.graphSampleRate == 96000 &&
                 !parsed.status.rateTransitioning &&
                 parsed.status.rateError.empty(),
             "parsed response rate status differs") ||
      !check(parsed.status.configuredDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 parsed.status.effectiveDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 parsed.status.configuredDspSimdVariant ==
                     pipetune::DspSimdVariant::x86_64_v3 &&
                 parsed.status.effectiveDspVariant ==
                     pipetune::DspBackendVariant::x86_64_v3 &&
                 !parsed.status.dspBackendFallback &&
                 parsed.status.dspBackendError.empty() &&
                 parsed.status.availableDspBackends[0].available &&
                 parsed.status.availableDspBackends[1].available,
             "parsed response DSP backend status differs") ||
      !check(parsed.warnings.size() == 1 &&
                 parsed.warnings.front().nodeIndex == 3 &&
                 parsed.warnings.front().pluginName == "Future DSP" &&
                 parsed.warnings.front().reason == "not available",
             "parsed response warnings differ")) {
    return false;
  }

  auto *document = yyjson_read(response.data(), response.size(), 0);
  if (!check(document != nullptr, "success response must be valid JSON")) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document);
  auto *warningArray = yyjson_obj_get(root, "warnings");
  const auto correct =
      yyjson_get_bool(yyjson_obj_get(root, "ok")) &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "processingMode"))) ==
          "preset" &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "dspActivity"))) ==
          "draining" &&
      yyjson_get_uint(
          yyjson_obj_get(root, "dspIdleTimeoutMilliseconds")) == 2500 &&
      std::string_view(yyjson_get_str(yyjson_obj_get(root, "preset"))) ==
          "/tmp/live.effetune_preset" &&
      yyjson_is_null(yyjson_obj_get(root, "configurationError")) &&
      yyjson_get_uint(yyjson_obj_get(root, "configurationRevision")) == 41 &&
      yyjson_get_uint(yyjson_obj_get(root, "activePluginCount")) == 7 &&
      yyjson_get_uint(yyjson_obj_get(root, "dspLatencyFrames")) == 64 &&
      yyjson_get_uint(yyjson_obj_get(root, "overrunFrames")) == 11 &&
      yyjson_get_uint(yyjson_obj_get(root, "underrunFrames")) == 12 &&
      yyjson_get_uint(yyjson_obj_get(root, "processingErrors")) == 13 &&
      yyjson_get_uint(yyjson_obj_get(root, "dspProcessedFrames")) == 96000 &&
      yyjson_get_uint(
          yyjson_obj_get(root, "dspProcessingNanoseconds")) == 240000000 &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "inputSampleFormat"))) ==
          "F32P" &&
      yyjson_get_uint(yyjson_obj_get(root, "inputSampleRate")) == 48000 &&
      yyjson_get_uint(yyjson_obj_get(root, "inputChannelCount")) == 2 &&
      yyjson_get_uint(yyjson_obj_get(root, "inputFramesReceived")) == 96000 &&
      yyjson_get_uint(
          yyjson_obj_get(root, "inputLastReceivedUnixMilliseconds")) ==
          1720000000123ULL &&
      std::string_view(yyjson_get_str(yyjson_obj_get(root, "rateMode"))) ==
          "fixed" &&
      yyjson_get_uint(yyjson_obj_get(root, "configuredSampleRate")) ==
          96000 &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "rateEnforcement"))) ==
          "force" &&
      yyjson_get_uint(yyjson_obj_get(root, "dspSampleRate")) == 96000 &&
      yyjson_get_uint(
          yyjson_obj_get(root, "graphSampleRate")) == 96000 &&
      !yyjson_get_bool(yyjson_obj_get(root, "rateTransitioning")) &&
      yyjson_is_null(yyjson_obj_get(root, "rateError")) &&
      std::string_view(yyjson_get_str(
          yyjson_obj_get(root, "configuredDspBackend"))) == "simd" &&
      std::string_view(yyjson_get_str(
          yyjson_obj_get(root, "configuredDspSimdVariant"))) ==
          "x86-64-v3" &&
      std::string_view(yyjson_get_str(
          yyjson_obj_get(root, "effectiveDspBackend"))) == "simd" &&
      std::string_view(yyjson_get_str(
          yyjson_obj_get(root, "effectiveDspVariant"))) ==
          "x86-64-v3" &&
      !yyjson_get_bool(
          yyjson_obj_get(root, "dspBackendFallback")) &&
      yyjson_is_null(yyjson_obj_get(root, "dspBackendError")) &&
      yyjson_is_arr(yyjson_obj_get(root, "availableDspBackends")) &&
      yyjson_arr_size(
          yyjson_obj_get(root, "availableDspBackends")) == 2 &&
      yyjson_get_bool(yyjson_obj_get(
          yyjson_arr_get(
              yyjson_obj_get(root, "availableDspBackends"), 1),
          "available")) &&
      yyjson_is_arr(yyjson_obj_get(root, "availableDspVariants")) &&
      yyjson_arr_size(
          yyjson_obj_get(root, "availableDspVariants")) == 3 &&
      yyjson_is_arr(warningArray) && yyjson_arr_size(warningArray) == 1 &&
      yyjson_get_uint(
          yyjson_obj_get(yyjson_arr_get(warningArray, 0), "nodeIndex")) == 3;
  yyjson_doc_free(document);
  return check(correct, "success response fields differ") &&
         check(
             !pipetune::parseControlResponse(mismatchedPinnedVariant).valid,
             "pinned DSP preference must reject a different effective tier");
}

static bool testStatusEvent() {
  const auto event = pipetune::makeControlStatusEvent(
      {.processingMode = pipetune::ProcessingMode::preset,
       .dspActivity = pipetune::DspActivity::sleeping,
       .dspIdlePolicy = {.timeoutMilliseconds = 100},
       .activePreset = "/tmp/event.effetune_preset",
       .configurationError = {},
       .configurationRevision = 42,
       .activePluginCount = 2,
       .dspLatencyFrames = 128,
       .overrunFrames = 21,
       .underrunFrames = 22,
       .processingErrors = 23,
       .dspProcessedFrames = 44100,
       .dspProcessingNanoseconds = 110250000,
       .inputSampleFormat = "F32P",
       .inputSampleRate = 44100,
       .inputChannelCount = 2,
       .inputFramesReceived = 44100,
       .inputLastReceivedUnixMilliseconds = 1720000001000,
       .configuredRatePolicy =
           {.mode = pipetune::SampleRateMode::automatic,
            .fixedRate = 0,
            .enforcement = pipetune::SampleRateEnforcement::suggest},
       .dspSampleRate = 192000,
       .graphSampleRate = 192000,
       .rateTransitioning = true,
       .rateError = "previous transition failed"});
  const auto parsed = pipetune::parseControlResponse(event);
  auto invalidTransition = event;
  auto invalidDspActivity = event;
  auto invalidDspIdleTimeout = event;
  auto missingRateError = event;
  auto missingRevision = event;
  if (!check(replaceOnce(invalidTransition,
                         R"json("rateTransitioning":true)json",
                         R"json("rateTransitioning":"true")json"),
             "cannot prepare invalid transition state") ||
      !check(replaceOnce(invalidDspActivity,
                         R"json("dspActivity":"sleeping")json",
                         R"json("dspActivity":"idle")json"),
             "cannot prepare invalid DSP activity") ||
      !check(replaceOnce(invalidDspIdleTimeout,
                         R"json("dspIdleTimeoutMilliseconds":100)json",
                         R"json("dspIdleTimeoutMilliseconds":150)json"),
             "cannot prepare invalid DSP idle timeout") ||
      !check(replaceOnce(missingRateError, "rateError",
                         "missingRateError"),
             "cannot prepare missing rate error") ||
      !check(replaceOnce(missingRevision, "configurationRevision",
                         "missingConfigurationRevision"),
             "cannot prepare missing configuration revision")) {
    return false;
  }
  return check(parsed.valid, parsed.error) &&
         check(parsed.success, "status event must report success") &&
         check(parsed.kind == pipetune::ControlResponseKind::statusEvent,
               "status event kind differs") &&
         check(parsed.status.processingMode ==
                   pipetune::ProcessingMode::preset,
               "status event processing mode differs") &&
         check(parsed.status.dspActivity ==
                       pipetune::DspActivity::sleeping &&
                   parsed.status.dspIdlePolicy.timeoutMilliseconds == 100,
               "status event DSP idle state differs") &&
         check(parsed.status.activePreset ==
                   "/tmp/event.effetune_preset",
               "status event preset differs") &&
         check(parsed.status.configurationRevision == 42,
               "status event configuration revision differs") &&
         check(parsed.status.activePluginCount == 2,
               "status event plugin count differs") &&
         check(parsed.status.dspLatencyFrames == 128,
               "status event DSP latency differs") &&
         check(parsed.status.overrunFrames == 21 &&
                   parsed.status.underrunFrames == 22 &&
                   parsed.status.processingErrors == 23,
               "status event counters differ") &&
         check(parsed.status.dspProcessedFrames == 44100 &&
                   parsed.status.dspProcessingNanoseconds == 110250000,
               "status event DSP performance counters differ") &&
         check(parsed.status.inputSampleFormat == "F32P" &&
                   parsed.status.inputSampleRate == 44100 &&
                   parsed.status.inputChannelCount == 2 &&
                   parsed.status.inputFramesReceived == 44100 &&
                   parsed.status.inputLastReceivedUnixMilliseconds ==
                       1720000001000ULL,
               "status event input telemetry differs") &&
         check(parsed.status.configuredRatePolicy.mode ==
                       pipetune::SampleRateMode::automatic &&
                   parsed.status.configuredRatePolicy.fixedRate == 0 &&
                   parsed.status.configuredRatePolicy.enforcement ==
                       pipetune::SampleRateEnforcement::suggest &&
                   parsed.status.dspSampleRate == 192000 &&
                   parsed.status.graphSampleRate == 192000 &&
                   parsed.status.rateTransitioning &&
                   parsed.status.rateError ==
                       "previous transition failed",
               "status event rate state differs") &&
         check(parsed.warnings.empty(),
               "status event must not contain warnings") &&
         check(!pipetune::parseControlResponse(invalidTransition).valid,
               "non-boolean rate transition state must be rejected") &&
         check(!pipetune::parseControlResponse(invalidDspActivity).valid,
               "unsupported DSP activity must be rejected") &&
         check(!pipetune::parseControlResponse(invalidDspIdleTimeout).valid,
               "invalid DSP idle timeout must be rejected") &&
         check(!pipetune::parseControlResponse(missingRateError).valid,
               "missing rate error field must be rejected") &&
         check(!pipetune::parseControlResponse(missingRevision).valid,
               "missing configuration revision must be rejected");
}

static bool testBypassStatus() {
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::bypass,
       .dspActivity = pipetune::DspActivity::bypassed,
       .dspIdlePolicy = {.timeoutMilliseconds = 0},
       .activePreset = {},
       .configurationError = "configured preset is unavailable",
       .activePluginCount = 0,
       .overrunFrames = 0,
       .underrunFrames = 0,
       .processingErrors = 0,
       .dspProcessedFrames = 0,
       .dspProcessingNanoseconds = 0,
       .inputSampleFormat = {},
       .inputSampleRate = 0,
       .inputChannelCount = 0,
       .inputFramesReceived = 0,
       .inputLastReceivedUnixMilliseconds = 0},
      {});
  const auto parsed = pipetune::parseControlResponse(response);
  if (!check(parsed.valid, parsed.error) ||
      !check(parsed.success, "bypass response must report success") ||
      !check(parsed.status.processingMode ==
                 pipetune::ProcessingMode::bypass,
             "bypass response must preserve its processing mode") ||
      !check(parsed.status.dspActivity ==
                     pipetune::DspActivity::bypassed &&
                 parsed.status.dspIdlePolicy.timeoutMilliseconds == 0,
             "bypass response DSP idle state differs") ||
      !check(parsed.status.activePreset.empty(),
             "bypass response must not report an active preset") ||
      !check(parsed.status.configurationError ==
                 "configured preset is unavailable",
             "bypass response must preserve the startup diagnostic") ||
      !check(parsed.status.activePluginCount == 0,
             "bypass response must report zero active plugins")) {
    return false;
  }

  auto *document = yyjson_read(response.data(), response.size(), 0);
  if (!check(document != nullptr, "bypass response must be valid JSON")) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document);
  const auto correct =
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "processingMode"))) ==
          "bypass" &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "dspActivity"))) ==
          "bypassed" &&
      yyjson_is_null(
          yyjson_obj_get(root, "dspIdleTimeoutMilliseconds")) &&
      yyjson_is_null(yyjson_obj_get(root, "preset")) &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "configurationError"))) ==
          "configured preset is unavailable" &&
      yyjson_is_null(yyjson_obj_get(root, "inputSampleFormat")) &&
      yyjson_get_uint(yyjson_obj_get(root, "inputSampleRate")) == 0 &&
      yyjson_get_uint(yyjson_obj_get(root, "inputChannelCount")) == 0 &&
      yyjson_get_uint(yyjson_obj_get(root, "inputFramesReceived")) == 0 &&
      yyjson_get_uint(
          yyjson_obj_get(root, "inputLastReceivedUnixMilliseconds")) == 0;
  yyjson_doc_free(document);
  return check(correct, "bypass response fields differ");
}

static bool testDspBackendFallbackStatus() {
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::bypass,
       .activePreset = {},
       .configurationError = {},
       .activePluginCount = 0,
       .overrunFrames = 0,
       .underrunFrames = 0,
       .processingErrors = 0,
       .dspProcessedFrames = 0,
       .dspProcessingNanoseconds = 0,
       .inputSampleFormat = {},
       .inputSampleRate = 0,
       .inputChannelCount = 0,
       .inputFramesReceived = 0,
       .inputLastReceivedUnixMilliseconds = 0,
       .configuredDspBackend = pipetune::DspBackendKind::simd,
       .configuredDspSimdVariant =
           pipetune::DspSimdVariant::x86_64_v4,
       .effectiveDspBackend = pipetune::DspBackendKind::scalar,
       .effectiveDspVariant = pipetune::DspBackendVariant::scalar,
       .dspBackendFallback = true,
       .dspBackendError = "x86-64-v4 is unavailable",
       .availableDspBackends =
           {{
               {.kind = pipetune::DspBackendKind::scalar,
                .available = true,
                .cpuRequirement = "none",
                .error = {}},
               {.kind = pipetune::DspBackendKind::simd,
                .available = true,
                .cpuRequirement = "test baseline ISA",
                .error = {}},
           }},
       .availableDspVariants =
           {{.variant = pipetune::DspBackendVariant::scalar,
             .available = true,
             .cpuSupported = true,
             .cpuRequirement = "none",
             .error = {}},
            {.variant = pipetune::DspBackendVariant::simdBaseline,
             .available = true,
             .cpuSupported = true,
             .cpuRequirement = "test baseline ISA",
             .error = {}},
            {.variant = pipetune::DspBackendVariant::x86_64_v4,
             .available = false,
             .cpuSupported = false,
             .cpuRequirement = "x86-64-v4",
             .error = "x86-64-v4 is unavailable"}}},
      {});
  const auto parsed = pipetune::parseControlResponse(response);
  auto inconsistent = response;
  if (!check(replaceOnce(inconsistent,
                         R"json("dspBackendFallback":true)json",
                         R"json("dspBackendFallback":false)json"),
             "cannot prepare inconsistent DSP backend fallback")) {
    return false;
  }
  return check(parsed.valid, parsed.error) &&
         check(parsed.status.configuredDspBackend ==
                   pipetune::DspBackendKind::simd,
               "fallback must retain configured SIMD") &&
         check(parsed.status.effectiveDspBackend ==
                   pipetune::DspBackendKind::scalar,
               "fallback must report effective scalar") &&
         check(parsed.status.dspBackendFallback,
               "fallback marker differs") &&
         check(parsed.status.dspBackendError ==
                   "x86-64-v4 is unavailable",
               "fallback diagnostic differs") &&
         check(parsed.status.availableDspBackends[1].available &&
                   !parsed.status.availableDspVariants[2].available,
               "pinned fallback must retain other available SIMD tiers") &&
         check(!pipetune::parseControlResponse(inconsistent).valid,
               "inconsistent DSP backend fallback must be rejected");
}

static bool testFixedDspAndGraphRatesCanDiffer() {
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::bypass,
       .activePreset = {},
       .configurationError = {},
       .activePluginCount = 0,
       .overrunFrames = 0,
       .underrunFrames = 0,
       .processingErrors = 0,
       .dspProcessedFrames = 0,
       .dspProcessingNanoseconds = 0,
       .inputSampleFormat = "F32P",
       .inputSampleRate = 192000,
       .inputChannelCount = 2,
       .inputFramesReceived = 0,
       .inputLastReceivedUnixMilliseconds = 0,
       .configuredRatePolicy =
            {.mode = pipetune::SampleRateMode::fixed,
             .fixedRate = 192000,
            .enforcement = pipetune::SampleRateEnforcement::suggest},
       .dspSampleRate = 192000,
       .graphSampleRate = 48000,
       .rateTransitioning = false,
       .rateError = {}},
      {});
  const auto parsed = pipetune::parseControlResponse(response);
  return check(parsed.valid, parsed.error) &&
         check(parsed.success,
               "fixed DSP and graph rates must be independently reportable") &&
         check(parsed.status.configuredRatePolicy.fixedRate == 192000 &&
                   parsed.status.dspSampleRate == 192000 &&
                   parsed.status.graphSampleRate == 48000,
               "fixed DSP rate must not follow a different graph rate") &&
         check(parsed.status.rateError.empty(),
               "different Suggest graph rate must not be an error");
}

static bool testRejectedInputTelemetry() {
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::bypass,
       .activePreset = {},
       .configurationError = {},
       .activePluginCount = 0,
       .overrunFrames = 0,
       .underrunFrames = 0,
       .processingErrors = 0,
       .dspProcessedFrames = 0,
       .dspProcessingNanoseconds = 0,
       .inputSampleFormat = "F32P",
       .inputSampleRate = 48000,
       .inputChannelCount = 2,
       .inputFramesReceived = 48000,
       .inputLastReceivedUnixMilliseconds = 1720000000000},
      {});
  auto outOfRangeRate = response;
  auto missingFormat = response;
  auto wrongCounterType = response;
  if (!check(replaceOnce(outOfRangeRate, R"json("inputSampleRate":48000)json",
                         R"json("inputSampleRate":4294967296)json"),
             "cannot prepare out-of-range input rate") ||
      !check(replaceOnce(missingFormat, "inputSampleFormat",
                         "missingInputSampleFormat"),
             "cannot prepare missing input format") ||
      !check(replaceOnce(wrongCounterType,
                         R"json("inputFramesReceived":48000)json",
                         R"json("inputFramesReceived":"48000")json"),
             "cannot prepare invalid input counter")) {
    return false;
  }
  return check(!pipetune::parseControlResponse(outOfRangeRate).valid,
               "out-of-range input rate must be rejected") &&
         check(!pipetune::parseControlResponse(missingFormat).valid,
               "missing input format must be rejected") &&
         check(!pipetune::parseControlResponse(wrongCounterType).valid,
               "non-numeric input counter must be rejected");
}

static bool testErrorResponse() {
  const auto response =
      pipetune::makeControlErrorResponse("cannot load \"broken\" preset");
  const auto inspection = pipetune::inspectControlResponse(response);
  const auto parsed = pipetune::parseControlResponse(response);
  return check(inspection.valid, inspection.error) &&
         check(!inspection.success, "error response must not report success") &&
         check(inspection.error == "cannot load \"broken\" preset",
               "error response diagnostic differs") &&
         check(parsed.valid, parsed.error) &&
         check(!parsed.success, "parsed error must not report success") &&
         check(parsed.error == "cannot load \"broken\" preset",
               "parsed error diagnostic differs");
}

int main() {
  return testRequests() && testRejectedRequests() && testSuccessResponse() &&
                 testStatusEvent() && testBypassStatus() &&
                 testDspBackendFallbackStatus() &&
                 testFixedDspAndGraphRatesCanDiffer() &&
                 testRejectedInputTelemetry() && testErrorResponse()
             ? 0
             : 1;
}
