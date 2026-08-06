#include "pipetune/control_protocol.h"

#include <yyjson.h>

#include <cstdlib>
#include <cstring>
#include <memory>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune {

struct JsonDocumentDeleter {
  void operator()(yyjson_doc *document) const noexcept {
    yyjson_doc_free(document);
  }
};

struct MutableJsonDocumentDeleter {
  void operator()(yyjson_mut_doc *document) const noexcept {
    yyjson_mut_doc_free(document);
  }
};

using JsonDocument = std::unique_ptr<yyjson_doc, JsonDocumentDeleter>;
using MutableJsonDocument =
    std::unique_ptr<yyjson_mut_doc, MutableJsonDocumentDeleter>;

constexpr auto kJsonWriteFlags =
    YYJSON_WRITE_ESCAPE_UNICODE | YYJSON_WRITE_ALLOW_INVALID_UNICODE;

static ControlRequestParseResult requestError(std::string message) {
  return {.request = {.command = ControlCommand::status,
                      .presetPath = {},
                      .ratePolicy = defaultSampleRatePolicy()},
          .error = std::move(message)};
}

static bool addString(yyjson_mut_doc *document, yyjson_mut_val *object,
                      const char *key, std::string_view value) {
  return yyjson_mut_obj_add_strncpy(document, object, key, value.data(),
                                    value.size());
}

static bool addNullableString(yyjson_mut_doc *document,
                              yyjson_mut_val *object, const char *key,
                              std::string_view value) {
  return value.empty() ? yyjson_mut_obj_add_null(document, object, key)
                       : addString(document, object, key, value);
}

static std::string writeDocument(yyjson_mut_doc *document) {
  auto length = std::size_t{0};
  auto *encoded = yyjson_mut_write(document, kJsonWriteFlags, &length);
  if (encoded == nullptr) {
    return {};
  }
  auto json = std::string(encoded, length);
  std::free(encoded);
  return json;
}

static MutableJsonDocument createObjectDocument(yyjson_mut_val *&root) {
  auto document = MutableJsonDocument(yyjson_mut_doc_new(nullptr));
  if (document == nullptr) {
    root = nullptr;
    return document;
  }
  root = yyjson_mut_obj(document.get());
  if (root == nullptr) {
    document.reset();
    return document;
  }
  yyjson_mut_doc_set_root(document.get(), root);
  return document;
}

ControlRequestParseResult parseControlRequest(std::string_view json) {
  if (json.empty()) {
    return requestError("control request must not be empty");
  }
  auto document =
      JsonDocument(yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG));
  if (document == nullptr) {
    return requestError("control request is not valid JSON");
  }
  auto *root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root)) {
    return requestError("control request root must be an object");
  }
  auto *commandValue = yyjson_obj_get(root, "command");
  if (!yyjson_is_str(commandValue)) {
    return requestError("control request command must be a string");
  }
  const auto command =
      std::string_view(yyjson_get_str(commandValue),
                       yyjson_get_len(commandValue));
  if (command == "status") {
    if (yyjson_obj_size(root) != 1) {
      return requestError("status request accepts only the command field");
    }
    return {.request = {.command = ControlCommand::status,
                        .presetPath = {},
                        .ratePolicy = defaultSampleRatePolicy()},
            .error = {}};
  }
  if (command == "bypass") {
    if (yyjson_obj_size(root) != 1) {
      return requestError("bypass request accepts only the command field");
    }
    return {.request = {.command = ControlCommand::bypass,
                        .presetPath = {},
                        .ratePolicy = defaultSampleRatePolicy()},
            .error = {}};
  }
  if (command == "subscribe") {
    if (yyjson_obj_size(root) != 1) {
      return requestError("subscribe request accepts only the command field");
    }
    return {.request = {.command = ControlCommand::subscribe,
                        .presetPath = {},
                        .ratePolicy = defaultSampleRatePolicy()},
            .error = {}};
  }
  if (command == "set-rate") {
    if (yyjson_obj_size(root) != 4) {
      return requestError(
          "set-rate request requires only command, rateMode, sampleRate, "
          "and enforcement fields");
    }
    auto *modeValue = yyjson_obj_get(root, "rateMode");
    auto *rateValue = yyjson_obj_get(root, "sampleRate");
    auto *enforcementValue = yyjson_obj_get(root, "enforcement");
    if (!yyjson_is_str(modeValue) || !yyjson_is_str(enforcementValue)) {
      return requestError(
          "set-rate request mode and enforcement must be strings");
    }
    auto policy = defaultSampleRatePolicy();
    const auto modeText = std::string_view(
        yyjson_get_str(modeValue), yyjson_get_len(modeValue));
    const auto enforcementText = std::string_view(
        yyjson_get_str(enforcementValue),
        yyjson_get_len(enforcementValue));
    if (!parseSampleRateMode(modeText, policy.mode) ||
        !parseSampleRateEnforcement(enforcementText,
                                    policy.enforcement)) {
      return requestError("set-rate request contains an unsupported policy");
    }
    if (policy.mode == SampleRateMode::automatic) {
      if (!yyjson_is_null(rateValue)) {
        return requestError(
            "set-rate automatic request sampleRate must be null");
      }
      policy.fixedRate = 0;
    } else {
      if (!yyjson_is_uint(rateValue) ||
          yyjson_get_uint(rateValue) >
              std::numeric_limits<std::uint32_t>::max()) {
        return requestError(
            "set-rate fixed request sampleRate must be an integer");
      }
      policy.fixedRate =
          static_cast<std::uint32_t>(yyjson_get_uint(rateValue));
    }
    if (!sampleRatePolicyIsValid(policy)) {
      return requestError("set-rate request contains an unsupported policy");
    }
    return {.request = {.command = ControlCommand::setRate,
                        .presetPath = {},
                        .ratePolicy = policy},
            .error = {}};
  }
  if (command == "set-dsp-backend") {
    if (yyjson_obj_size(root) != 3) {
      return requestError(
          "set-dsp-backend request requires only command, backend, and "
          "simdVariant fields");
    }
    auto *backendValue = yyjson_obj_get(root, "backend");
    auto *variantValue = yyjson_obj_get(root, "simdVariant");
    if (!yyjson_is_str(backendValue) || !yyjson_is_str(variantValue)) {
      return requestError(
          "set-dsp-backend request backend and simdVariant must be strings");
    }
    const auto backend = parseDspBackendName(
        std::string_view(yyjson_get_str(backendValue),
                         yyjson_get_len(backendValue)));
    if (!backend.has_value()) {
      return requestError(
          "set-dsp-backend request contains an unsupported backend");
    }
    const auto simdVariant = parseDspSimdVariantName(
        std::string_view(yyjson_get_str(variantValue),
                         yyjson_get_len(variantValue)));
    if (!simdVariant.has_value() ||
        (*backend == DspBackendKind::scalar &&
         *simdVariant != DspSimdVariant::automatic)) {
      return requestError(
          "set-dsp-backend request contains an unsupported SIMD variant");
    }
    return {.request = {.command = ControlCommand::setDspBackend,
                        .presetPath = {},
                        .ratePolicy = defaultSampleRatePolicy(),
                        .dspBackend = *backend,
                        .dspSimdVariant = *simdVariant},
            .error = {}};
  }
  if (command != "load") {
    return requestError("unsupported control command");
  }
  if (yyjson_obj_size(root) != 2) {
    return requestError(
        "load request requires only command and preset fields");
  }
  auto *presetValue = yyjson_obj_get(root, "preset");
  if (!yyjson_is_str(presetValue) || yyjson_get_len(presetValue) == 0) {
    return requestError("load request preset must be a non-empty string");
  }
  auto preset =
      std::string(yyjson_get_str(presetValue), yyjson_get_len(presetValue));
  if (preset.find('\0') != std::string::npos) {
    return requestError("load request preset must not contain NUL");
  }
  return {.request = {.command = ControlCommand::loadPreset,
                      .presetPath = std::move(preset),
                      .ratePolicy = defaultSampleRatePolicy()},
          .error = {}};
}

std::string makeStatusControlRequest() {
  return R"json({"command":"status"})json";
}

std::string makeSubscribeControlRequest() {
  return R"json({"command":"subscribe"})json";
}

std::string makeBypassControlRequest() {
  return R"json({"command":"bypass"})json";
}

std::string makeSetRateControlRequest(const SampleRatePolicy &policy) {
  if (!sampleRatePolicyIsValid(policy)) {
    return {};
  }
  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  if (document == nullptr ||
      !yyjson_mut_obj_add_str(document.get(), root, "command", "set-rate") ||
      !addString(document.get(), root, "rateMode",
                 sampleRateModeName(policy.mode)) ||
      (policy.mode == SampleRateMode::automatic
           ? !yyjson_mut_obj_add_null(document.get(), root, "sampleRate")
           : !yyjson_mut_obj_add_uint(document.get(), root, "sampleRate",
                                      policy.fixedRate)) ||
      !addString(document.get(), root, "enforcement",
                 sampleRateEnforcementName(policy.enforcement))) {
    return {};
  }
  return writeDocument(document.get());
}

std::string makeSetDspBackendControlRequest(DspBackendKind kind) {
  return makeSetDspBackendControlRequest(kind,
                                         DspSimdVariant::automatic);
}

std::string
makeSetDspBackendControlRequest(DspBackendKind kind,
                                DspSimdVariant simdVariant) {
  if (kind != DspBackendKind::scalar && kind != DspBackendKind::simd) {
    return {};
  }
  if (dspSimdVariantName(simdVariant).empty() ||
      (kind == DspBackendKind::scalar &&
       simdVariant != DspSimdVariant::automatic)) {
    return {};
  }
  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  if (document == nullptr ||
      !yyjson_mut_obj_add_str(document.get(), root, "command",
                              "set-dsp-backend") ||
      !addString(document.get(), root, "backend",
                 dspBackendName(kind)) ||
      !addString(document.get(), root, "simdVariant",
                 dspSimdVariantName(simdVariant))) {
    return {};
  }
  return writeDocument(document.get());
}

std::string
makeLoadPresetControlRequest(const std::filesystem::path &presetPath) {
  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  const auto path = presetPath.string();
  if (document == nullptr ||
      !yyjson_mut_obj_add_str(document.get(), root, "command", "load") ||
      !addString(document.get(), root, "preset", path)) {
    return {};
  }
  return writeDocument(document.get());
}

static bool dspBackendStatusIsConsistent(
    const ControlRuntimeStatus &status) noexcept {
  const auto &scalar = status.availableDspBackends[0];
  const auto &simd = status.availableDspBackends[1];
  if (scalar.kind != DspBackendKind::scalar ||
      simd.kind != DspBackendKind::simd ||
      scalar.cpuRequirement.empty() || simd.cpuRequirement.empty() ||
      scalar.cpuRequirement.find('\0') != std::string::npos ||
      simd.cpuRequirement.find('\0') != std::string::npos ||
      scalar.error.find('\0') != std::string::npos ||
      simd.error.find('\0') != std::string::npos ||
      scalar.available != scalar.error.empty() ||
      simd.available != simd.error.empty() ||
      status.dspBackendError.find('\0') != std::string::npos ||
      dspSimdVariantName(status.configuredDspSimdVariant).empty()) {
    return false;
  }
  const ControlDspVariantAvailability *effectiveAvailability = nullptr;
  auto scalarVariantAvailable = false;
  auto simdVariantAvailable = false;
  for (auto index = std::size_t{0};
       index < status.availableDspVariants.size(); ++index) {
    const auto &variant = status.availableDspVariants[index];
    if (variant.cpuRequirement.empty() ||
        variant.cpuRequirement.find('\0') != std::string::npos ||
        variant.error.find('\0') != std::string::npos ||
        variant.available != variant.error.empty() ||
        (variant.available && !variant.cpuSupported)) {
      return false;
    }
    for (auto earlier = std::size_t{0}; earlier < index; ++earlier) {
      if (status.availableDspVariants[earlier].variant ==
          variant.variant) {
        return false;
      }
    }
    scalarVariantAvailable |=
        variant.variant == DspBackendVariant::scalar &&
        variant.available;
    simdVariantAvailable |=
        dspBackendKind(variant.variant) == DspBackendKind::simd &&
        variant.available;
    if (status.effectiveDspVariant == variant.variant) {
      effectiveAvailability = &variant;
    }
  }
  if (scalar.available != scalarVariantAvailable ||
      simd.available != simdVariantAvailable ||
      status.effectiveDspBackend.has_value() !=
          status.effectiveDspVariant.has_value() ||
      (status.effectiveDspVariant.has_value() &&
       (!status.effectiveDspBackend.has_value() ||
        dspBackendKind(*status.effectiveDspVariant) !=
            *status.effectiveDspBackend ||
        effectiveAvailability == nullptr ||
        !effectiveAvailability->available))) {
    return false;
  }
  if (!scalar.available) {
    return !status.effectiveDspBackend.has_value() &&
           !status.effectiveDspVariant.has_value() &&
           !status.dspBackendFallback &&
           !status.dspBackendError.empty();
  }
  if (status.configuredDspBackend == DspBackendKind::scalar) {
    return status.effectiveDspBackend == DspBackendKind::scalar &&
           status.effectiveDspVariant == DspBackendVariant::scalar &&
           !status.dspBackendFallback &&
           status.dspBackendError.empty();
  }
  if (status.configuredDspBackend != DspBackendKind::simd) {
    return false;
  }
  if (status.effectiveDspBackend == DspBackendKind::simd) {
    const auto pinned = concreteDspBackendVariant(
        status.configuredDspSimdVariant);
    return simd.available &&
           (!pinned.has_value() ||
            status.effectiveDspVariant == pinned) &&
           (status.dspBackendFallback ==
            !status.dspBackendError.empty()) &&
           (!status.dspBackendFallback ||
            status.configuredDspSimdVariant ==
                DspSimdVariant::automatic);
  }
  return status.effectiveDspBackend == DspBackendKind::scalar &&
         status.effectiveDspVariant == DspBackendVariant::scalar &&
         status.dspBackendFallback && !status.dspBackendError.empty();
}

static std::string makeControlStatusMessage(
    const ControlRuntimeStatus &status,
    std::span<const ControlWarning> warnings, bool statusEvent) {
  if ((status.processingMode == ProcessingMode::preset &&
       status.activePreset.empty()) ||
      (status.processingMode == ProcessingMode::bypass &&
       !status.activePreset.empty()) ||
      !sampleRatePolicyIsValid(status.configuredRatePolicy) ||
      (status.configuredRatePolicy.mode == SampleRateMode::fixed &&
       status.configuredRatePolicy.enforcement ==
           SampleRateEnforcement::force &&
       !status.rateTransitioning && status.dspSampleRate != 0 &&
       status.dspSampleRate != status.configuredRatePolicy.fixedRate &&
       status.rateError.empty()) ||
      (status.graphSampleRate != 0 &&
       status.graphSampleRate != status.dspSampleRate) ||
      status.rateError.find('\0') != std::string::npos ||
      !dspBackendStatusIsConsistent(status)) {
    return makeControlErrorResponse("cannot encode inconsistent control status");
  }

  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  const auto processingMode =
      status.processingMode == ProcessingMode::bypass ? "bypass" : "preset";
  if (document == nullptr ||
      !yyjson_mut_obj_add_bool(document.get(), root, "ok", true) ||
      (statusEvent &&
       !yyjson_mut_obj_add_str(document.get(), root, "event", "status")) ||
      !yyjson_mut_obj_add_str(document.get(), root, "processingMode",
                              processingMode) ||
      !addNullableString(document.get(), root, "preset",
                         status.activePreset) ||
      !addNullableString(document.get(), root, "configurationError",
                         status.configurationError) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "activePluginCount",
                               status.activePluginCount) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "overrunFrames",
                               status.overrunFrames) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "underrunFrames",
                               status.underrunFrames) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "processingErrors",
                               status.processingErrors) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "dspProcessedFrames",
                               status.dspProcessedFrames) ||
      !yyjson_mut_obj_add_uint(
          document.get(), root, "dspProcessingNanoseconds",
          status.dspProcessingNanoseconds) ||
      !addNullableString(document.get(), root, "inputSampleFormat",
                         status.inputSampleFormat) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "inputSampleRate",
                               status.inputSampleRate) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "inputChannelCount",
                               status.inputChannelCount) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "inputFramesReceived",
                               status.inputFramesReceived) ||
      !yyjson_mut_obj_add_uint(
          document.get(), root, "inputLastReceivedUnixMilliseconds",
          status.inputLastReceivedUnixMilliseconds) ||
      !addString(document.get(), root, "rateMode",
                 sampleRateModeName(status.configuredRatePolicy.mode)) ||
      (status.configuredRatePolicy.mode == SampleRateMode::automatic
           ? !yyjson_mut_obj_add_null(document.get(), root,
                                      "configuredSampleRate")
           : !yyjson_mut_obj_add_uint(
                 document.get(), root, "configuredSampleRate",
                 status.configuredRatePolicy.fixedRate)) ||
      !addString(
          document.get(), root, "rateEnforcement",
          sampleRateEnforcementName(
              status.configuredRatePolicy.enforcement)) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "dspSampleRate",
                               status.dspSampleRate) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "graphSampleRate",
                               status.graphSampleRate) ||
      !yyjson_mut_obj_add_bool(document.get(), root, "rateTransitioning",
                               status.rateTransitioning) ||
      !addNullableString(document.get(), root, "rateError",
                         status.rateError) ||
      !addString(document.get(), root, "configuredDspBackend",
                 dspBackendName(status.configuredDspBackend)) ||
      !addString(document.get(), root, "configuredDspSimdVariant",
                 dspSimdVariantName(status.configuredDspSimdVariant)) ||
      (status.effectiveDspBackend.has_value()
           ? !addString(document.get(), root, "effectiveDspBackend",
                        dspBackendName(*status.effectiveDspBackend))
           : !yyjson_mut_obj_add_null(document.get(), root,
                                      "effectiveDspBackend")) ||
      (status.effectiveDspVariant.has_value()
           ? !addString(document.get(), root, "effectiveDspVariant",
                        dspBackendVariantName(
                            *status.effectiveDspVariant))
           : !yyjson_mut_obj_add_null(document.get(), root,
                                      "effectiveDspVariant")) ||
      !yyjson_mut_obj_add_bool(document.get(), root,
                               "dspBackendFallback",
                               status.dspBackendFallback) ||
      !addNullableString(document.get(), root, "dspBackendError",
                         status.dspBackendError)) {
    return makeControlErrorResponse("cannot encode control response");
  }

  auto *backendArray =
      yyjson_mut_obj_add_arr(document.get(), root, "availableDspBackends");
  if (backendArray == nullptr) {
    return makeControlErrorResponse("cannot encode control response");
  }
  for (const auto &backend : status.availableDspBackends) {
    auto *item = yyjson_mut_obj(document.get());
    if (item == nullptr || !yyjson_mut_arr_append(backendArray, item) ||
        !addString(document.get(), item, "name",
                   dspBackendName(backend.kind)) ||
        !yyjson_mut_obj_add_bool(document.get(), item, "available",
                                 backend.available) ||
        !addString(document.get(), item, "cpuRequirement",
                   backend.cpuRequirement) ||
        !addNullableString(document.get(), item, "error",
                           backend.error)) {
      return makeControlErrorResponse("cannot encode control response");
    }
  }

  auto *variantArray =
      yyjson_mut_obj_add_arr(document.get(), root, "availableDspVariants");
  if (variantArray == nullptr) {
    return makeControlErrorResponse("cannot encode control response");
  }
  for (const auto &variant : status.availableDspVariants) {
    auto *item = yyjson_mut_obj(document.get());
    if (item == nullptr || !yyjson_mut_arr_append(variantArray, item) ||
        !addString(document.get(), item, "name",
                   dspBackendVariantName(variant.variant)) ||
        !yyjson_mut_obj_add_bool(document.get(), item, "available",
                                 variant.available) ||
        !yyjson_mut_obj_add_bool(document.get(), item, "cpuSupported",
                                 variant.cpuSupported) ||
        !addString(document.get(), item, "cpuRequirement",
                   variant.cpuRequirement) ||
        !addNullableString(document.get(), item, "error",
                           variant.error)) {
      return makeControlErrorResponse("cannot encode control response");
    }
  }

  auto *warningArray =
      yyjson_mut_obj_add_arr(document.get(), root, "warnings");
  if (warningArray == nullptr) {
    return makeControlErrorResponse("cannot encode control response");
  }
  for (const auto &warning : warnings) {
    auto *item = yyjson_mut_obj(document.get());
    if (item == nullptr || !yyjson_mut_arr_append(warningArray, item) ||
        !yyjson_mut_obj_add_uint(document.get(), item, "nodeIndex",
                                 warning.nodeIndex) ||
        !addString(document.get(), item, "pluginName", warning.pluginName) ||
        !addString(document.get(), item, "reason", warning.reason)) {
      return makeControlErrorResponse("cannot encode control response");
    }
  }

  const auto encoded = writeDocument(document.get());
  return encoded.empty()
             ? makeControlErrorResponse("cannot encode control response")
             : encoded;
}

std::string makeControlSuccessResponse(
    const ControlRuntimeStatus &status,
    std::span<const ControlWarning> warnings) {
  return makeControlStatusMessage(status, warnings, false);
}

std::string makeControlStatusEvent(const ControlRuntimeStatus &status) {
  return makeControlStatusMessage(
      status, std::span<const ControlWarning>{}, true);
}

std::string makeControlErrorResponse(std::string_view error) {
  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  if (document == nullptr ||
      !yyjson_mut_obj_add_bool(document.get(), root, "ok", false) ||
      !addString(document.get(), root, "error", error)) {
    return R"json({"ok":false,"error":"cannot encode control response"})json";
  }
  const auto encoded = writeDocument(document.get());
  return encoded.empty()
             ? R"json({"ok":false,"error":"cannot encode control response"})json"
             : encoded;
}

static ControlResponseParseResult responseError(std::string error) {
  return {.valid = false,
          .success = false,
          .kind = ControlResponseKind::response,
          .status = {.processingMode = ProcessingMode::bypass,
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
                     .inputLastReceivedUnixMilliseconds = 0},
          .warnings = {},
          .error = std::move(error)};
}

static bool readStringField(yyjson_val *object, const char *key,
                            std::string &value) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_str(field)) {
    return false;
  }
  value.assign(yyjson_get_str(field), yyjson_get_len(field));
  return true;
}

static bool readNullableStringField(yyjson_val *object, const char *key,
                                    std::string &value) {
  auto *field = yyjson_obj_get(object, key);
  if (yyjson_is_null(field)) {
    value.clear();
    return true;
  }
  if (!yyjson_is_str(field)) {
    return false;
  }
  value.assign(yyjson_get_str(field), yyjson_get_len(field));
  return true;
}

static bool readProcessingMode(yyjson_val *object, ProcessingMode &mode) {
  auto *field = yyjson_obj_get(object, "processingMode");
  if (!yyjson_is_str(field)) {
    return false;
  }
  const auto value =
      std::string_view(yyjson_get_str(field), yyjson_get_len(field));
  if (value == "bypass") {
    mode = ProcessingMode::bypass;
    return true;
  }
  if (value == "preset") {
    mode = ProcessingMode::preset;
    return true;
  }
  return false;
}

static bool readDspBackendKindField(yyjson_val *object, const char *key,
                                    DspBackendKind &kind) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_str(field)) {
    return false;
  }
  const auto parsed = parseDspBackendName(
      std::string_view(yyjson_get_str(field), yyjson_get_len(field)));
  if (!parsed.has_value()) {
    return false;
  }
  kind = *parsed;
  return true;
}

static bool readNullableDspBackendKindField(
    yyjson_val *object, const char *key,
    std::optional<DspBackendKind> &kind) {
  auto *field = yyjson_obj_get(object, key);
  if (yyjson_is_null(field)) {
    kind.reset();
    return true;
  }
  if (!yyjson_is_str(field)) {
    return false;
  }
  const auto parsed = parseDspBackendName(
      std::string_view(yyjson_get_str(field), yyjson_get_len(field)));
  if (!parsed.has_value()) {
    return false;
  }
  kind = *parsed;
  return true;
}

static bool readDspSimdVariantField(
    yyjson_val *object, const char *key, DspSimdVariant &variant) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_str(field)) {
    return false;
  }
  const auto parsed = parseDspSimdVariantName(
      std::string_view(yyjson_get_str(field), yyjson_get_len(field)));
  if (!parsed.has_value()) {
    return false;
  }
  variant = *parsed;
  return true;
}

static bool readDspBackendVariantField(
    yyjson_val *object, const char *key, DspBackendVariant &variant) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_str(field)) {
    return false;
  }
  const auto parsed = parseDspBackendVariantName(
      std::string_view(yyjson_get_str(field), yyjson_get_len(field)));
  if (!parsed.has_value()) {
    return false;
  }
  variant = *parsed;
  return true;
}

static bool readNullableDspBackendVariantField(
    yyjson_val *object, const char *key,
    std::optional<DspBackendVariant> &variant) {
  auto *field = yyjson_obj_get(object, key);
  if (yyjson_is_null(field)) {
    variant.reset();
    return true;
  }
  auto parsed = DspBackendVariant::scalar;
  if (!readDspBackendVariantField(object, key, parsed)) {
    return false;
  }
  variant = parsed;
  return true;
}

static bool readSampleRatePolicy(yyjson_val *object,
                                 SampleRatePolicy &policy) {
  auto *modeField = yyjson_obj_get(object, "rateMode");
  auto *rateField = yyjson_obj_get(object, "configuredSampleRate");
  auto *enforcementField = yyjson_obj_get(object, "rateEnforcement");
  if (!yyjson_is_str(modeField) || !yyjson_is_str(enforcementField) ||
      !parseSampleRateMode(
          std::string_view(yyjson_get_str(modeField),
                           yyjson_get_len(modeField)),
          policy.mode) ||
      !parseSampleRateEnforcement(
          std::string_view(yyjson_get_str(enforcementField),
                           yyjson_get_len(enforcementField)),
          policy.enforcement)) {
    return false;
  }
  if (policy.mode == SampleRateMode::automatic) {
    if (!yyjson_is_null(rateField)) {
      return false;
    }
    policy.fixedRate = 0;
  } else {
    if (!yyjson_is_uint(rateField) ||
        yyjson_get_uint(rateField) >
            static_cast<std::uint64_t>(
                std::numeric_limits<std::uint32_t>::max())) {
      return false;
    }
    policy.fixedRate =
        static_cast<std::uint32_t>(yyjson_get_uint(rateField));
  }
  return sampleRatePolicyIsValid(policy);
}

static bool readBooleanField(yyjson_val *object, const char *key,
                             bool &value) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_bool(field)) {
    return false;
  }
  value = yyjson_get_bool(field);
  return true;
}

static bool readUint32Field(yyjson_val *object, const char *key,
                            std::uint32_t &value);

static bool readAvailableDspBackends(
    yyjson_val *object,
    std::array<ControlDspBackendAvailability, 2> &backends) {
  auto *array = yyjson_obj_get(object, "availableDspBackends");
  if (!yyjson_is_arr(array) || yyjson_arr_size(array) != backends.size()) {
    return false;
  }
  for (auto index = std::size_t{0}; index < backends.size(); ++index) {
    auto *item = yyjson_arr_get(array, index);
    auto backend = ControlDspBackendAvailability{
        .kind = DspBackendKind::scalar,
        .available = false,
        .cpuRequirement = {},
        .error = {},
    };
    if (!yyjson_is_obj(item) || yyjson_obj_size(item) != 4 ||
        !readDspBackendKindField(item, "name", backend.kind) ||
        !readBooleanField(item, "available", backend.available) ||
        !readStringField(item, "cpuRequirement",
                         backend.cpuRequirement) ||
        !readNullableStringField(item, "error", backend.error)) {
      return false;
    }
    backends[index] = std::move(backend);
  }
  return true;
}

static bool readAvailableDspVariants(
    yyjson_val *object,
    std::vector<ControlDspVariantAvailability> &variants) {
  auto *array = yyjson_obj_get(object, "availableDspVariants");
  if (!yyjson_is_arr(array)) {
    return false;
  }
  variants.clear();
  variants.reserve(yyjson_arr_size(array));
  for (auto index = std::size_t{0}; index < yyjson_arr_size(array);
       ++index) {
    auto *item = yyjson_arr_get(array, index);
    auto variant = ControlDspVariantAvailability{
        .variant = DspBackendVariant::scalar,
        .available = false,
        .cpuSupported = false,
        .cpuRequirement = {},
        .error = {},
    };
    if (!yyjson_is_obj(item) || yyjson_obj_size(item) != 5 ||
        !readDspBackendVariantField(item, "name", variant.variant) ||
        !readBooleanField(item, "available", variant.available) ||
        !readBooleanField(item, "cpuSupported", variant.cpuSupported) ||
        !readStringField(item, "cpuRequirement",
                         variant.cpuRequirement) ||
        !readNullableStringField(item, "error", variant.error)) {
      return false;
    }
    variants.push_back(std::move(variant));
  }
  return true;
}

static bool readSizeField(yyjson_val *object, const char *key,
                          std::size_t &value) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_uint(field) ||
      yyjson_get_uint(field) >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::size_t>::max())) {
    return false;
  }
  value = static_cast<std::size_t>(yyjson_get_uint(field));
  return true;
}

static bool readCounterField(yyjson_val *object, const char *key,
                             std::uint64_t &value) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_uint(field)) {
    return false;
  }
  value = yyjson_get_uint(field);
  return true;
}

static bool readUint32Field(yyjson_val *object, const char *key,
                            std::uint32_t &value) {
  auto *field = yyjson_obj_get(object, key);
  if (!yyjson_is_uint(field) ||
      yyjson_get_uint(field) >
          static_cast<std::uint64_t>(
              std::numeric_limits<std::uint32_t>::max())) {
    return false;
  }
  value = static_cast<std::uint32_t>(yyjson_get_uint(field));
  return true;
}

ControlResponseParseResult parseControlResponse(std::string_view json) {
  auto document =
      JsonDocument(yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG));
  if (document == nullptr) {
    return responseError("control response is not valid JSON");
  }
  auto *root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root)) {
    return responseError("control response root must be an object");
  }
  auto *ok = yyjson_obj_get(root, "ok");
  if (!yyjson_is_bool(ok)) {
    return responseError("control response lacks a boolean ok field");
  }

  auto kind = ControlResponseKind::response;
  auto *event = yyjson_obj_get(root, "event");
  if (event != nullptr) {
    if (!yyjson_is_str(event) ||
        std::string_view(yyjson_get_str(event), yyjson_get_len(event)) !=
            "status") {
      return responseError("control response has an unsupported event");
    }
    kind = ControlResponseKind::statusEvent;
  }

  if (!yyjson_get_bool(ok)) {
    auto error = std::string{};
    if (kind != ControlResponseKind::response) {
      return responseError("failed control response must not be an event");
    }
    if (!readStringField(root, "error", error)) {
      return responseError("failed control response lacks an error string");
    }
    return {.valid = true,
            .success = false,
            .kind = kind,
            .status = {.processingMode = ProcessingMode::bypass,
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
                       .inputLastReceivedUnixMilliseconds = 0},
            .warnings = {},
            .error = std::move(error)};
  }

  auto status = ControlRuntimeStatus{
      .processingMode = ProcessingMode::bypass,
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
  };
  if (!readProcessingMode(root, status.processingMode) ||
      !readNullableStringField(root, "preset", status.activePreset) ||
      !readNullableStringField(root, "configurationError",
                               status.configurationError) ||
      !readSizeField(root, "activePluginCount",
                     status.activePluginCount) ||
      !readCounterField(root, "overrunFrames", status.overrunFrames) ||
      !readCounterField(root, "underrunFrames", status.underrunFrames) ||
      !readCounterField(root, "processingErrors",
                        status.processingErrors) ||
      !readCounterField(root, "dspProcessedFrames",
                        status.dspProcessedFrames) ||
      !readCounterField(root, "dspProcessingNanoseconds",
                        status.dspProcessingNanoseconds) ||
      !readNullableStringField(root, "inputSampleFormat",
                               status.inputSampleFormat) ||
      !readUint32Field(root, "inputSampleRate", status.inputSampleRate) ||
      !readUint32Field(root, "inputChannelCount",
                       status.inputChannelCount) ||
      !readCounterField(root, "inputFramesReceived",
                        status.inputFramesReceived) ||
      !readCounterField(root, "inputLastReceivedUnixMilliseconds",
                        status.inputLastReceivedUnixMilliseconds) ||
      !readSampleRatePolicy(root, status.configuredRatePolicy) ||
      !readUint32Field(root, "dspSampleRate", status.dspSampleRate) ||
      !readUint32Field(root, "graphSampleRate",
                       status.graphSampleRate) ||
      !readBooleanField(root, "rateTransitioning",
                        status.rateTransitioning) ||
      !readNullableStringField(root, "rateError",
                               status.rateError) ||
      !readDspBackendKindField(root, "configuredDspBackend",
                               status.configuredDspBackend) ||
      !readDspSimdVariantField(root, "configuredDspSimdVariant",
                               status.configuredDspSimdVariant) ||
      !readNullableDspBackendKindField(root, "effectiveDspBackend",
                                       status.effectiveDspBackend) ||
      !readNullableDspBackendVariantField(root, "effectiveDspVariant",
                                          status.effectiveDspVariant) ||
      !readBooleanField(root, "dspBackendFallback",
                        status.dspBackendFallback) ||
      !readNullableStringField(root, "dspBackendError",
                               status.dspBackendError) ||
      !readAvailableDspBackends(root, status.availableDspBackends) ||
      !readAvailableDspVariants(root, status.availableDspVariants)) {
    return responseError("successful control response has invalid status");
  }
  if ((status.processingMode == ProcessingMode::preset &&
       status.activePreset.empty()) ||
      (status.processingMode == ProcessingMode::bypass &&
       !status.activePreset.empty()) ||
      !sampleRatePolicyIsValid(status.configuredRatePolicy) ||
      (status.configuredRatePolicy.mode == SampleRateMode::fixed &&
       status.configuredRatePolicy.enforcement ==
           SampleRateEnforcement::force &&
       !status.rateTransitioning && status.dspSampleRate != 0 &&
       status.dspSampleRate != status.configuredRatePolicy.fixedRate &&
       status.rateError.empty()) ||
      (status.graphSampleRate != 0 &&
       status.graphSampleRate != status.dspSampleRate) ||
      status.rateError.find('\0') != std::string::npos ||
      !dspBackendStatusIsConsistent(status)) {
    return responseError(
        "successful control response has inconsistent status");
  }
  const auto hasInputFormat = !status.inputSampleFormat.empty();
  if (hasInputFormat !=
          (status.inputSampleRate != 0 && status.inputChannelCount != 0) ||
      (status.inputFramesReceived == 0) !=
          (status.inputLastReceivedUnixMilliseconds == 0)) {
    return responseError(
        "successful control response has inconsistent input telemetry");
  }
  auto *warningArray = yyjson_obj_get(root, "warnings");
  if (!yyjson_is_arr(warningArray)) {
    return responseError(
        "successful control response lacks a warnings array");
  }
  auto warnings = std::vector<ControlWarning>{};
  warnings.reserve(yyjson_arr_size(warningArray));
  for (auto index = std::size_t{0}; index < yyjson_arr_size(warningArray);
       ++index) {
    auto *item = yyjson_arr_get(warningArray, index);
    auto warning = ControlWarning{
        .nodeIndex = 0,
        .pluginName = {},
        .reason = {},
    };
    if (!yyjson_is_obj(item) ||
        !readSizeField(item, "nodeIndex", warning.nodeIndex) ||
        !readStringField(item, "pluginName", warning.pluginName) ||
        !readStringField(item, "reason", warning.reason)) {
      return responseError(
          "successful control response has an invalid warning");
    }
    warnings.push_back(std::move(warning));
  }
  return {.valid = true,
          .success = true,
          .kind = kind,
          .status = std::move(status),
          .warnings = std::move(warnings),
          .error = {}};
}

ControlResponseInspection inspectControlResponse(std::string_view json) {
  auto document =
      JsonDocument(yyjson_read(json.data(), json.size(), YYJSON_READ_NOFLAG));
  if (document == nullptr) {
    return {.valid = false,
            .success = false,
            .error = "control response is not valid JSON"};
  }
  auto *root = yyjson_doc_get_root(document.get());
  auto *ok = yyjson_is_obj(root) ? yyjson_obj_get(root, "ok") : nullptr;
  if (!yyjson_is_bool(ok)) {
    return {.valid = false,
            .success = false,
            .error = "control response lacks a boolean ok field"};
  }
  if (yyjson_get_bool(ok)) {
    return {.valid = true, .success = true, .error = {}};
  }
  auto *error = yyjson_obj_get(root, "error");
  if (!yyjson_is_str(error)) {
    return {.valid = false,
            .success = false,
            .error = "failed control response lacks an error string"};
  }
  return {.valid = true,
          .success = false,
          .error = std::string(yyjson_get_str(error), yyjson_get_len(error))};
}

} // namespace pipetune
