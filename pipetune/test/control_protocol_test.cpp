#include "pipetune/control_protocol.h"

#include <array>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static pipetune::ControlFilterOutputStatus activeOutput() {
  return {
      .targetNodeName = "alsa_output.usb",
      .targetDescription = "USB DAC",
      .filterNodeName = "pipetune.filter.usb",
      .state = pipetune::ControlFilterState::active,
      .error = {},
      .channelCount = 2,
      .sampleRateCapabilities =
          {.known = true,
           .constraints =
               {{.kind = pipetune::SampleRateConstraintKind::discrete,
                 .minimum = 96000,
                 .maximum = 96000,
                 .step = 0}}},
      .dspSampleRate = 96000,
      .outputSampleRate = 96000,
      .activeOutputSampleRate = 96000,
      .rateFallback = false,
      .latencyFrames = 64,
      .overrunFrames = 2,
      .underrunFrames = 3,
      .processingErrors = 1,
      .dspProcessedFrames = 96000,
      .dspProcessingNanoseconds = 4000000,
  };
}

static pipetune::ControlRuntimeStatus runtimeStatus() {
  auto status = pipetune::ControlRuntimeStatus{
      .processingMode = pipetune::ProcessingMode::preset,
      .activePreset = "/tmp/live.effetune_preset",
      .configurationError = {},
      .activePluginCount = 3,
      .policyBackend = "wireplumber-0.5",
      .filterOutputs = {activeOutput()},
      .overrunFrames = 2,
      .underrunFrames = 3,
      .processingErrors = 1,
      .dspProcessedFrames = 96000,
      .dspProcessingNanoseconds = 4000000,
      .configuredRatePolicy =
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 96000,
           .enforcement = pipetune::SampleRateEnforcement::force},
  };
  return status;
}

static bool testRequests() {
  const auto fixedPolicy = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::force};
  const auto requests = std::array{
      std::pair{pipetune::makeStatusControlRequest(),
                pipetune::ControlCommand::status},
      std::pair{pipetune::makeSubscribeControlRequest(),
                pipetune::ControlCommand::subscribe},
      std::pair{pipetune::makeBypassControlRequest(),
                pipetune::ControlCommand::bypass},
      std::pair{pipetune::makeLoadPresetControlRequest(
                    "/tmp/a \"wide\".effetune_preset"),
                pipetune::ControlCommand::loadPreset},
      std::pair{pipetune::makeSetRateControlRequest(fixedPolicy),
                pipetune::ControlCommand::setRate},
      std::pair{pipetune::makeSetDspBackendControlRequest(
                    pipetune::DspBackendKind::simd,
                    pipetune::DspSimdVariant::x86_64_v3),
                pipetune::ControlCommand::setDspBackend},
  };
  for (const auto &[encoded, expected] : requests) {
    const auto parsed = pipetune::parseControlRequest(encoded);
    if (!check(!encoded.empty(), "control request encoder returned empty") ||
        !check(parsed.error.empty(), parsed.error) ||
        !check(parsed.request.command == expected,
               "control request command did not round-trip")) {
      return false;
    }
  }

  const auto load = pipetune::parseControlRequest(requests[3].first);
  const auto rate = pipetune::parseControlRequest(requests[4].first);
  const auto backend = pipetune::parseControlRequest(requests[5].first);
  return check(load.request.presetPath ==
                   "/tmp/a \"wide\".effetune_preset",
               "load request path differs") &&
         check(rate.request.ratePolicy == fixedPolicy,
               "rate request policy differs") &&
         check(backend.request.dspBackend ==
                       pipetune::DspBackendKind::simd &&
                   backend.request.dspSimdVariant ==
                       pipetune::DspSimdVariant::x86_64_v3,
               "backend request selection differs");
}

static bool testRejectedRequests() {
  const auto invalidJson = pipetune::parseControlRequest("{");
  const auto oldVersion = pipetune::parseControlRequest(
      R"json({"protocolVersion":1,"command":"status"})json");
  const auto extraStatus = pipetune::parseControlRequest(
      R"json({"protocolVersion":2,"command":"status","extra":true})json");
  const auto emptyLoad = pipetune::parseControlRequest(
      R"json({"protocolVersion":2,"command":"load","preset":""})json");
  const auto invalidRate = pipetune::parseControlRequest(
      R"json({"protocolVersion":2,"command":"set-rate","rateMode":"fixed","sampleRate":88200,"enforcement":"suggest"})json");
  const auto invalidScalarVariant = pipetune::parseControlRequest(
      R"json({"protocolVersion":2,"command":"set-dsp-backend","backend":"scalar","simdVariant":"baseline"})json");
  return check(!invalidJson.error.empty(), "invalid JSON must be rejected") &&
         check(!oldVersion.error.empty(),
               "old protocol versions must be rejected") &&
         check(!extraStatus.error.empty(),
               "extra status fields must be rejected") &&
         check(!emptyLoad.error.empty(), "empty presets must be rejected") &&
         check(!invalidRate.error.empty(),
               "unsupported rates must be rejected") &&
         check(!invalidScalarVariant.error.empty(),
               "scalar backend cannot pin a SIMD variant") &&
         check(pipetune::makeSetRateControlRequest(
                   {.mode = pipetune::SampleRateMode::fixed,
                    .fixedRate = 88200,
                    .enforcement =
                        pipetune::SampleRateEnforcement::suggest})
                   .empty(),
               "invalid rate policies must not be encoded");
}

static bool testSuccessAndEventResponses() {
  const auto status = runtimeStatus();
  const auto warnings = std::array<pipetune::ControlWarning, 1>{
      pipetune::ControlWarning{.nodeIndex = 2,
                               .pluginName = "Unsupported Plugin",
                               .reason = "not available"}};
  const auto encoded =
      pipetune::makeControlSuccessResponse(status, warnings);
  const auto parsed = pipetune::parseControlResponse(encoded);
  if (!check(parsed.valid, parsed.error) ||
      !check(parsed.success &&
                 parsed.kind == pipetune::ControlResponseKind::response,
             "successful response envelope differs") ||
      !check(parsed.status.activePreset == status.activePreset &&
                 parsed.status.policyBackend == status.policyBackend &&
                 parsed.status.filterOutputs.size() == 1 &&
                 parsed.status.filterOutputs[0].targetNodeName ==
                     "alsa_output.usb",
             "runtime status did not round-trip") ||
      !check(parsed.warnings.size() == 1 &&
                 parsed.warnings[0].nodeIndex == 2 &&
                 parsed.warnings[0].pluginName == "Unsupported Plugin",
             "control warnings did not round-trip")) {
    return false;
  }

  const auto event = pipetune::parseControlResponse(
      pipetune::makeControlStatusEvent(status));
  return check(event.valid && event.success &&
                   event.kind ==
                       pipetune::ControlResponseKind::statusEvent &&
                   event.warnings.empty(),
               "status event did not round-trip");
}

static bool testErrorAndInvalidResponses() {
  const auto encoded = pipetune::makeControlErrorResponse("load failed");
  const auto parsed = pipetune::parseControlResponse(encoded);
  const auto inspected = pipetune::inspectControlResponse(encoded);
  if (!check(parsed.valid && !parsed.success &&
                 parsed.error == "load failed",
             "error response did not round-trip") ||
      !check(inspected.valid && !inspected.success &&
                 inspected.error == "load failed",
             "error response inspection differs")) {
    return false;
  }

  const auto invalid = pipetune::parseControlResponse(
      R"json({"protocolVersion":2,"ok":true})json");
  auto inconsistent = runtimeStatus();
  inconsistent.filterOutputs[0].filterNodeName.clear();
  const auto consistencyError = pipetune::parseControlResponse(
      pipetune::makeControlSuccessResponse(inconsistent, {}));
  return check(!invalid.valid,
               "incomplete success responses must be rejected") &&
         check(consistencyError.valid && !consistencyError.success &&
                   consistencyError.error.find("inconsistent") !=
                       std::string::npos,
               "inconsistent filter status must encode as an error");
}

int main() {
  return testRequests() && testRejectedRequests() &&
                 testSuccessAndEventResponses() &&
                 testErrorAndInvalidResponses()
             ? 0
             : 1;
}
