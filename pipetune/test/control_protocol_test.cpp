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
  return check(load.error.empty(), load.error) &&
         check(load.request.command == pipetune::ControlCommand::loadPreset,
               "load request command differs") &&
         check(load.request.presetPath ==
                   "/tmp/music \"wide\".effetune_preset",
               "load request preset differs");
}

static bool testRejectedRequests() {
  constexpr auto inputs = std::array<std::string_view, 8>{
      "",
      "[]",
      R"json({"command":"unknown"})json",
      R"json({"command":"status","preset":"unexpected"})json",
      R"json({"command":"bypass","preset":"unexpected"})json",
      R"json({"command":"load"})json",
      R"json({"command":"load","preset":42})json",
      R"json({"command":"load","preset":""})json"};
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
       .activePreset = "/tmp/live.effetune_preset",
       .configurationError = {},
       .activePluginCount = 7,
       .selectedTarget = "alsa_output.speaker",
       .defaultSinkActive = true,
       .overrunFrames = 11,
       .underrunFrames = 12,
       .processingErrors = 13,
       .inputSampleFormat = "F32P",
       .inputSampleRate = 48000,
       .inputChannelCount = 2,
       .inputFramesReceived = 96000,
       .inputLastReceivedUnixMilliseconds = 1720000000123},
      warnings);
  const auto inspection = pipetune::inspectControlResponse(response);
  const auto parsed = pipetune::parseControlResponse(response);
  if (!check(inspection.valid, inspection.error) ||
      !check(inspection.success, "success response must report success") ||
      !check(parsed.valid, parsed.error) ||
      !check(parsed.success, "parsed response must report success") ||
      !check(parsed.kind == pipetune::ControlResponseKind::response,
             "ordinary response kind differs") ||
      !check(parsed.status.processingMode ==
                 pipetune::ProcessingMode::preset,
             "parsed response processing mode differs") ||
      !check(parsed.status.activePreset ==
                 "/tmp/live.effetune_preset",
             "parsed response preset differs") ||
      !check(parsed.status.activePluginCount == 7,
             "parsed response plugin count differs") ||
      !check(parsed.status.selectedTarget == "alsa_output.speaker",
             "parsed response target differs") ||
      !check(parsed.status.defaultSinkActive,
             "parsed response default state differs") ||
      !check(parsed.status.overrunFrames == 11 &&
                 parsed.status.underrunFrames == 12 &&
                 parsed.status.processingErrors == 13,
             "parsed response counters differ") ||
      !check(parsed.status.inputSampleFormat == "F32P" &&
                 parsed.status.inputSampleRate == 48000 &&
                 parsed.status.inputChannelCount == 2,
             "parsed response input format differs") ||
      !check(parsed.status.inputFramesReceived == 96000 &&
                 parsed.status.inputLastReceivedUnixMilliseconds ==
                     1720000000123ULL,
             "parsed response input telemetry differs") ||
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
      std::string_view(yyjson_get_str(yyjson_obj_get(root, "preset"))) ==
          "/tmp/live.effetune_preset" &&
      yyjson_is_null(yyjson_obj_get(root, "configurationError")) &&
      yyjson_get_uint(yyjson_obj_get(root, "activePluginCount")) == 7 &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "selectedTarget"))) ==
          "alsa_output.speaker" &&
      yyjson_get_bool(yyjson_obj_get(root, "defaultSinkActive")) &&
      yyjson_get_uint(yyjson_obj_get(root, "overrunFrames")) == 11 &&
      yyjson_get_uint(yyjson_obj_get(root, "underrunFrames")) == 12 &&
      yyjson_get_uint(yyjson_obj_get(root, "processingErrors")) == 13 &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "inputSampleFormat"))) ==
          "F32P" &&
      yyjson_get_uint(yyjson_obj_get(root, "inputSampleRate")) == 48000 &&
      yyjson_get_uint(yyjson_obj_get(root, "inputChannelCount")) == 2 &&
      yyjson_get_uint(yyjson_obj_get(root, "inputFramesReceived")) == 96000 &&
      yyjson_get_uint(
          yyjson_obj_get(root, "inputLastReceivedUnixMilliseconds")) ==
          1720000000123ULL &&
      yyjson_is_arr(warningArray) && yyjson_arr_size(warningArray) == 1 &&
      yyjson_get_uint(
          yyjson_obj_get(yyjson_arr_get(warningArray, 0), "nodeIndex")) == 3;
  yyjson_doc_free(document);
  return check(correct, "success response fields differ");
}

static bool testStatusEvent() {
  const auto event = pipetune::makeControlStatusEvent(
      {.processingMode = pipetune::ProcessingMode::preset,
       .activePreset = "/tmp/event.effetune_preset",
       .configurationError = {},
       .activePluginCount = 2,
       .selectedTarget = "alsa_output.headphones",
       .defaultSinkActive = false,
       .overrunFrames = 21,
       .underrunFrames = 22,
       .processingErrors = 23,
       .inputSampleFormat = "F32P",
       .inputSampleRate = 44100,
       .inputChannelCount = 2,
       .inputFramesReceived = 44100,
       .inputLastReceivedUnixMilliseconds = 1720000001000});
  const auto parsed = pipetune::parseControlResponse(event);
  return check(parsed.valid, parsed.error) &&
         check(parsed.success, "status event must report success") &&
         check(parsed.kind == pipetune::ControlResponseKind::statusEvent,
               "status event kind differs") &&
         check(parsed.status.processingMode ==
                   pipetune::ProcessingMode::preset,
               "status event processing mode differs") &&
         check(parsed.status.activePreset ==
                   "/tmp/event.effetune_preset",
               "status event preset differs") &&
         check(parsed.status.activePluginCount == 2,
               "status event plugin count differs") &&
         check(parsed.status.selectedTarget ==
                   "alsa_output.headphones",
               "status event target differs") &&
         check(!parsed.status.defaultSinkActive,
               "status event default state differs") &&
         check(parsed.status.overrunFrames == 21 &&
                   parsed.status.underrunFrames == 22 &&
                   parsed.status.processingErrors == 23,
               "status event counters differ") &&
         check(parsed.status.inputSampleFormat == "F32P" &&
                   parsed.status.inputSampleRate == 44100 &&
                   parsed.status.inputChannelCount == 2 &&
                   parsed.status.inputFramesReceived == 44100 &&
                   parsed.status.inputLastReceivedUnixMilliseconds ==
                       1720000001000ULL,
               "status event input telemetry differs") &&
         check(parsed.warnings.empty(),
               "status event must not contain warnings");
}

static bool testBypassStatus() {
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::bypass,
       .activePreset = {},
       .configurationError = "configured preset is unavailable",
       .activePluginCount = 0,
       .selectedTarget = "alsa_output.speaker",
       .defaultSinkActive = true,
       .overrunFrames = 0,
       .underrunFrames = 0,
       .processingErrors = 0,
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

static bool testRejectedInputTelemetry() {
  const auto response = pipetune::makeControlSuccessResponse(
      {.processingMode = pipetune::ProcessingMode::bypass,
       .activePreset = {},
       .configurationError = {},
       .activePluginCount = 0,
       .selectedTarget = "alsa_output.speaker",
       .defaultSinkActive = true,
       .overrunFrames = 0,
       .underrunFrames = 0,
       .processingErrors = 0,
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
                 testRejectedInputTelemetry() && testErrorResponse()
             ? 0
             : 1;
}
