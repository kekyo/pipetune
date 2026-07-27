#include "control_protocol.h"

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
  constexpr auto inputs = std::array<std::string_view, 7>{
      "",
      "[]",
      R"json({"command":"unknown"})json",
      R"json({"command":"status","preset":"unexpected"})json",
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
  const auto warnings = std::array<pipetune::PipelineWarning, 1>{
      pipetune::PipelineWarning{.nodeIndex = 3,
                                .pluginName = "Future DSP",
                                .reason = "not available"}};
  const auto response = pipetune::makeControlSuccessResponse(
      {.activePreset = "/tmp/live.effetune_preset",
       .activePluginCount = 7,
       .selectedTarget = "alsa_output.speaker",
       .defaultSinkActive = true,
       .overrunFrames = 11,
       .underrunFrames = 12,
       .processingErrors = 13},
      warnings);
  const auto inspection = pipetune::inspectControlResponse(response);
  if (!check(inspection.valid, inspection.error) ||
      !check(inspection.success, "success response must report success")) {
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
      std::string_view(yyjson_get_str(yyjson_obj_get(root, "preset"))) ==
          "/tmp/live.effetune_preset" &&
      yyjson_get_uint(yyjson_obj_get(root, "activePluginCount")) == 7 &&
      std::string_view(
          yyjson_get_str(yyjson_obj_get(root, "selectedTarget"))) ==
          "alsa_output.speaker" &&
      yyjson_get_bool(yyjson_obj_get(root, "defaultSinkActive")) &&
      yyjson_get_uint(yyjson_obj_get(root, "overrunFrames")) == 11 &&
      yyjson_get_uint(yyjson_obj_get(root, "underrunFrames")) == 12 &&
      yyjson_get_uint(yyjson_obj_get(root, "processingErrors")) == 13 &&
      yyjson_is_arr(warningArray) && yyjson_arr_size(warningArray) == 1 &&
      yyjson_get_uint(
          yyjson_obj_get(yyjson_arr_get(warningArray, 0), "nodeIndex")) == 3;
  yyjson_doc_free(document);
  return check(correct, "success response fields differ");
}

static bool testErrorResponse() {
  const auto response =
      pipetune::makeControlErrorResponse("cannot load \"broken\" preset");
  const auto inspection = pipetune::inspectControlResponse(response);
  return check(inspection.valid, inspection.error) &&
         check(!inspection.success, "error response must not report success") &&
         check(inspection.error == "cannot load \"broken\" preset",
               "error response diagnostic differs");
}

int main() {
  return testRequests() && testRejectedRequests() && testSuccessResponse() &&
                 testErrorResponse()
             ? 0
             : 1;
}
