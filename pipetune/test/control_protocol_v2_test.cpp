#include "pipetune/control_protocol.h"

#include <yyjson.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlRuntimeStatus makeStatus() {
  return {
      .processingMode = pipetune::ProcessingMode::preset,
      .activePreset = "/tmp/live.effetune_preset",
      .configurationError = {},
      .activePluginCount = 4,
      .policyBackend = "wireplumber-0.5",
      .filterOutputs =
          {{.targetNodeName = "alsa_output.speakers",
            .targetDescription = "Built-in Speakers",
            .filterNodeName = "pipetune.filter.41",
            .state = pipetune::ControlFilterState::active,
            .error = {},
            .channelCount = 2,
            .sampleRateCapabilities =
                {.known = true,
                 .constraints =
                     {{.kind =
                           pipetune::SampleRateConstraintKind::discrete,
                       .minimum = 48000,
                       .maximum = 48000,
                       .step = 0}}},
            .dspSampleRate = 96000,
            .outputSampleRate = 48000,
            .activeOutputSampleRate = 48000,
            .rateFallback = true,
            .latencyFrames = 128,
            .overrunFrames = 3,
            .underrunFrames = 5,
            .processingErrors = 1,
            .dspProcessedFrames = 96000,
            .dspProcessingNanoseconds = 2000000},
           {.targetNodeName = "bluez_output.headphones",
            .targetDescription = "Headphones",
            .filterNodeName = {},
            .state = pipetune::ControlFilterState::bypassed,
            .error = "unsupported channel layout",
            .channelCount = 0,
            .sampleRateCapabilities = {},
            .dspSampleRate = 0,
            .outputSampleRate = 0,
            .activeOutputSampleRate = 0,
            .rateFallback = false,
            .latencyFrames = 0,
            .overrunFrames = 0,
            .underrunFrames = 0,
            .processingErrors = 0,
            .dspProcessedFrames = 0,
            .dspProcessingNanoseconds = 0}},
      .preferredTarget = {},
      .selectedTarget = {},
      .outputSelectionReason =
          pipetune::ControlOutputSelectionReason::unavailable,
      .availableOutputs = {},
      .defaultSinkActive = false,
      .overrunFrames = 3,
      .underrunFrames = 5,
      .processingErrors = 1,
      .dspProcessedFrames = 96000,
      .dspProcessingNanoseconds = 2000000,
      .inputSampleFormat = "F32P",
      .inputSampleRate = 96000,
      .inputChannelCount = 2,
      .inputFramesReceived = 96000,
      .inputLastReceivedUnixMilliseconds = 1720000000123,
      .configuredRatePolicy =
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 96000,
           .enforcement = pipetune::SampleRateEnforcement::suggest}};
}

static bool testVersionedRequests() {
  const auto request = pipetune::makeStatusControlRequest();
  auto *document = yyjson_read(request.data(), request.size(), 0);
  if (!check(document != nullptr, "v2 status request must be valid JSON")) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document);
  const auto version = yyjson_obj_get(root, "protocolVersion");
  const auto command = yyjson_obj_get(root, "command");
  const auto valid =
      yyjson_is_uint(version) &&
      yyjson_get_uint(version) == pipetune::kControlProtocolVersion &&
      yyjson_is_str(command) &&
      std::string_view(yyjson_get_str(command), yyjson_get_len(command)) ==
          "status";
  yyjson_doc_free(document);
  const auto parsed = pipetune::parseControlRequest(request);
  return check(valid, "control requests must declare protocol version 2") &&
         check(parsed.error.empty(), parsed.error) &&
         check(parsed.request.command == pipetune::ControlCommand::status,
               "v2 status request must round-trip");
}

static bool testFilterStatusRoundTrip() {
  const auto response = pipetune::makeControlSuccessResponse(makeStatus(), {});
  const auto parsed = pipetune::parseControlResponse(response);
  if (!check(parsed.valid, parsed.error) ||
      !check(parsed.success, "v2 response must report success") ||
      !check(parsed.status.policyBackend == "wireplumber-0.5",
             "policy backend must round-trip") ||
      !check(parsed.status.filterOutputs.size() == 2,
             "every observed physical output must round-trip")) {
    return false;
  }
  const auto &active = parsed.status.filterOutputs[0];
  const auto &bypassed = parsed.status.filterOutputs[1];
  if (!check(active.targetNodeName == "alsa_output.speakers" &&
                 active.state == pipetune::ControlFilterState::active &&
                 active.channelCount == 2 &&
                 active.dspSampleRate == 96000 &&
                 active.outputSampleRate == 48000 &&
                 active.activeOutputSampleRate == 48000 &&
                 active.rateFallback && active.latencyFrames == 128,
             "active per-output filter state differs") ||
      !check(active.sampleRateCapabilities.known &&
                 pipetune::sampleRateCapabilitiesSupport(
                     active.sampleRateCapabilities, 48000),
             "per-output rate capabilities differ") ||
      !check(active.overrunFrames == 3 && active.underrunFrames == 5 &&
                 active.processingErrors == 1 &&
                 active.dspProcessedFrames == 96000 &&
                 active.dspProcessingNanoseconds == 2000000,
             "per-output counters differ") ||
      !check(bypassed.targetNodeName == "bluez_output.headphones" &&
                 bypassed.state ==
                     pipetune::ControlFilterState::bypassed &&
                 !bypassed.error.empty(),
             "fail-open output state differs")) {
    return false;
  }

  auto *document = yyjson_read(response.data(), response.size(), 0);
  if (!check(document != nullptr, "v2 response must be valid JSON")) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document);
  auto *outputs = yyjson_obj_get(root, "filterOutputs");
  const auto valid =
      yyjson_get_uint(yyjson_obj_get(root, "protocolVersion")) ==
          pipetune::kControlProtocolVersion &&
      yyjson_is_arr(outputs) && yyjson_arr_size(outputs) == 2 &&
      std::string_view(yyjson_get_str(yyjson_obj_get(
          yyjson_arr_get(outputs, 0), "state"))) == "active" &&
      std::string_view(yyjson_get_str(yyjson_obj_get(
          yyjson_arr_get(outputs, 1), "state"))) == "bypassed";
  yyjson_doc_free(document);
  return check(valid, "v2 response JSON must expose filter outputs");
}

static bool testAllFilterStates() {
  constexpr auto states = std::array<pipetune::ControlFilterState, 4>{
      pipetune::ControlFilterState::waiting,
      pipetune::ControlFilterState::active,
      pipetune::ControlFilterState::bypassed,
      pipetune::ControlFilterState::error};
  auto status = makeStatus();
  for (const auto state : states) {
    status.filterOutputs[0].state = state;
    status.filterOutputs[0].error =
        state == pipetune::ControlFilterState::error ? "failed" : "";
    const auto parsed = pipetune::parseControlResponse(
        pipetune::makeControlSuccessResponse(status, {}));
    if (!check(parsed.valid && parsed.success &&
                   parsed.status.filterOutputs[0].state == state,
               "filter state must round-trip")) {
      return false;
    }
  }
  return true;
}

int main() {
  return testVersionedRequests() && testFilterStatusRoundTrip() &&
                 testAllFilterStates()
             ? 0
             : 1;
}
