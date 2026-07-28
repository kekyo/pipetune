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
                      .outputTarget = {}},
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
                        .outputTarget = {}},
            .error = {}};
  }
  if (command == "bypass") {
    if (yyjson_obj_size(root) != 1) {
      return requestError("bypass request accepts only the command field");
    }
    return {.request = {.command = ControlCommand::bypass,
                        .presetPath = {},
                        .outputTarget = {}},
            .error = {}};
  }
  if (command == "clear-output") {
    if (yyjson_obj_size(root) != 1) {
      return requestError(
          "clear-output request accepts only the command field");
    }
    return {.request = {.command = ControlCommand::clearOutput,
                        .presetPath = {},
                        .outputTarget = {}},
            .error = {}};
  }
  if (command == "subscribe") {
    if (yyjson_obj_size(root) != 1) {
      return requestError("subscribe request accepts only the command field");
    }
    return {.request = {.command = ControlCommand::subscribe,
                        .presetPath = {},
                        .outputTarget = {}},
            .error = {}};
  }
  if (command == "set-output") {
    if (yyjson_obj_size(root) != 2) {
      return requestError(
          "set-output request requires only command and target fields");
    }
    auto *targetValue = yyjson_obj_get(root, "target");
    if (!yyjson_is_str(targetValue) || yyjson_get_len(targetValue) == 0) {
      return requestError(
          "set-output request target must be a non-empty string");
    }
    auto target =
        std::string(yyjson_get_str(targetValue), yyjson_get_len(targetValue));
    if (target.find('\0') != std::string::npos ||
        target.find('\n') != std::string::npos ||
        target.find('\r') != std::string::npos) {
      return requestError(
          "set-output request target must be a single line without NUL");
    }
    return {.request = {.command = ControlCommand::setOutput,
                        .presetPath = {},
                        .outputTarget = std::move(target)},
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
                      .outputTarget = {}},
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

std::string makeSetOutputControlRequest(std::string_view nodeName) {
  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  if (document == nullptr ||
      !yyjson_mut_obj_add_str(document.get(), root, "command",
                              "set-output") ||
      !addString(document.get(), root, "target", nodeName)) {
    return {};
  }
  return writeDocument(document.get());
}

std::string makeClearOutputControlRequest() {
  return R"json({"command":"clear-output"})json";
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

static const char *outputSelectionReasonName(
    ControlOutputSelectionReason reason) noexcept {
  switch (reason) {
  case ControlOutputSelectionReason::unavailable:
    return "unavailable";
  case ControlOutputSelectionReason::systemDefault:
    return "system-default";
  case ControlOutputSelectionReason::preferred:
    return "preferred";
  case ControlOutputSelectionReason::fallback:
    return "fallback";
  }
  return nullptr;
}

static bool outputStatusIsConsistent(
    const ControlRuntimeStatus &status) noexcept {
  const auto reason =
      outputSelectionReasonName(status.outputSelectionReason);
  if (reason == nullptr) {
    return false;
  }
  switch (status.outputSelectionReason) {
  case ControlOutputSelectionReason::unavailable:
    if (!status.selectedTarget.empty() ||
        !status.availableOutputs.empty()) {
      return false;
    }
    break;
  case ControlOutputSelectionReason::systemDefault:
    if (!status.preferredTarget.empty() ||
        status.selectedTarget.empty()) {
      return false;
    }
    break;
  case ControlOutputSelectionReason::preferred:
    if (status.preferredTarget.empty() ||
        status.preferredTarget != status.selectedTarget) {
      return false;
    }
    break;
  case ControlOutputSelectionReason::fallback:
    if (status.preferredTarget.empty() ||
        status.selectedTarget.empty() ||
        status.preferredTarget == status.selectedTarget) {
      return false;
    }
    break;
  }

  auto selectedCount = std::size_t{0};
  auto systemDefaultCount = std::size_t{0};
  for (auto index = std::size_t{0}; index < status.availableOutputs.size();
       ++index) {
    const auto &output = status.availableOutputs[index];
    if (output.name.empty() || output.description.empty() ||
        output.selected != (output.name == status.selectedTarget) ||
        output.preferred !=
            (!status.preferredTarget.empty() &&
             output.name == status.preferredTarget)) {
      return false;
    }
    selectedCount += output.selected ? 1U : 0U;
    systemDefaultCount += output.systemDefault ? 1U : 0U;
    for (auto earlier = std::size_t{0}; earlier < index; ++earlier) {
      if (status.availableOutputs[earlier].name == output.name) {
        return false;
      }
    }
  }
  return systemDefaultCount <= 1 &&
         selectedCount == (status.selectedTarget.empty() ? 0U : 1U);
}

static std::string makeControlStatusMessage(
    const ControlRuntimeStatus &status,
    std::span<const ControlWarning> warnings, bool statusEvent) {
  if ((status.processingMode == ProcessingMode::preset &&
       status.activePreset.empty()) ||
      (status.processingMode == ProcessingMode::bypass &&
       !status.activePreset.empty()) ||
      !outputStatusIsConsistent(status)) {
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
      !addNullableString(document.get(), root, "preferredTarget",
                         status.preferredTarget) ||
      !addNullableString(document.get(), root, "selectedTarget",
                         status.selectedTarget) ||
      !yyjson_mut_obj_add_str(
          document.get(), root, "outputSelectionReason",
          outputSelectionReasonName(status.outputSelectionReason)) ||
      !yyjson_mut_obj_add_bool(document.get(), root, "defaultSinkActive",
                               status.defaultSinkActive) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "overrunFrames",
                               status.overrunFrames) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "underrunFrames",
                               status.underrunFrames) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "processingErrors",
                               status.processingErrors) ||
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
          status.inputLastReceivedUnixMilliseconds)) {
    return makeControlErrorResponse("cannot encode control response");
  }

  auto *outputArray =
      yyjson_mut_obj_add_arr(document.get(), root, "availableOutputs");
  if (outputArray == nullptr) {
    return makeControlErrorResponse("cannot encode control response");
  }
  for (const auto &output : status.availableOutputs) {
    auto *item = yyjson_mut_obj(document.get());
    if (item == nullptr || !yyjson_mut_arr_append(outputArray, item) ||
        !addString(document.get(), item, "name", output.name) ||
        !addString(document.get(), item, "description",
                   output.description) ||
        !yyjson_mut_obj_add_bool(document.get(), item, "systemDefault",
                                 output.systemDefault) ||
        !yyjson_mut_obj_add_bool(document.get(), item, "preferred",
                                 output.preferred) ||
        !yyjson_mut_obj_add_bool(document.get(), item, "selected",
                                 output.selected)) {
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
                     .preferredTarget = {},
                     .selectedTarget = {},
                     .outputSelectionReason =
                         ControlOutputSelectionReason::unavailable,
                     .availableOutputs = {},
                     .defaultSinkActive = false,
                     .overrunFrames = 0,
                     .underrunFrames = 0,
                     .processingErrors = 0,
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

static bool readOutputSelectionReason(
    yyjson_val *object, ControlOutputSelectionReason &reason) {
  auto *field = yyjson_obj_get(object, "outputSelectionReason");
  if (!yyjson_is_str(field)) {
    return false;
  }
  const auto value =
      std::string_view(yyjson_get_str(field), yyjson_get_len(field));
  if (value == "unavailable") {
    reason = ControlOutputSelectionReason::unavailable;
    return true;
  }
  if (value == "system-default") {
    reason = ControlOutputSelectionReason::systemDefault;
    return true;
  }
  if (value == "preferred") {
    reason = ControlOutputSelectionReason::preferred;
    return true;
  }
  if (value == "fallback") {
    reason = ControlOutputSelectionReason::fallback;
    return true;
  }
  return false;
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

static bool readAvailableOutputs(
    yyjson_val *object, std::vector<ControlOutputDevice> &outputs) {
  auto *array = yyjson_obj_get(object, "availableOutputs");
  if (!yyjson_is_arr(array)) {
    return false;
  }
  outputs.clear();
  outputs.reserve(yyjson_arr_size(array));
  for (auto index = std::size_t{0}; index < yyjson_arr_size(array);
       ++index) {
    auto *item = yyjson_arr_get(array, index);
    auto output = ControlOutputDevice{
        .name = {},
        .description = {},
        .systemDefault = false,
        .preferred = false,
        .selected = false,
    };
    if (!yyjson_is_obj(item) ||
        !readStringField(item, "name", output.name) ||
        !readStringField(item, "description", output.description) ||
        !readBooleanField(item, "systemDefault",
                          output.systemDefault) ||
        !readBooleanField(item, "preferred", output.preferred) ||
        !readBooleanField(item, "selected", output.selected) ||
        output.name.find('\0') != std::string::npos ||
        output.description.find('\0') != std::string::npos) {
      return false;
    }
    outputs.push_back(std::move(output));
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
                       .preferredTarget = {},
                       .selectedTarget = {},
                       .outputSelectionReason =
                           ControlOutputSelectionReason::unavailable,
                       .availableOutputs = {},
                       .defaultSinkActive = false,
                       .overrunFrames = 0,
                       .underrunFrames = 0,
                       .processingErrors = 0,
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
      .preferredTarget = {},
      .selectedTarget = {},
      .outputSelectionReason =
          ControlOutputSelectionReason::unavailable,
      .availableOutputs = {},
      .defaultSinkActive = false,
      .overrunFrames = 0,
      .underrunFrames = 0,
      .processingErrors = 0,
      .inputSampleFormat = {},
      .inputSampleRate = 0,
      .inputChannelCount = 0,
      .inputFramesReceived = 0,
      .inputLastReceivedUnixMilliseconds = 0,
  };
  auto *defaultSinkActive = yyjson_obj_get(root, "defaultSinkActive");
  if (!readProcessingMode(root, status.processingMode) ||
      !readNullableStringField(root, "preset", status.activePreset) ||
      !readNullableStringField(root, "configurationError",
                               status.configurationError) ||
      !readSizeField(root, "activePluginCount",
                     status.activePluginCount) ||
      !readNullableStringField(root, "preferredTarget",
                               status.preferredTarget) ||
      !readNullableStringField(root, "selectedTarget",
                               status.selectedTarget) ||
      !readOutputSelectionReason(root, status.outputSelectionReason) ||
      !readAvailableOutputs(root, status.availableOutputs) ||
      !yyjson_is_bool(defaultSinkActive) ||
      !readCounterField(root, "overrunFrames", status.overrunFrames) ||
      !readCounterField(root, "underrunFrames", status.underrunFrames) ||
      !readCounterField(root, "processingErrors",
                        status.processingErrors) ||
      !readNullableStringField(root, "inputSampleFormat",
                               status.inputSampleFormat) ||
      !readUint32Field(root, "inputSampleRate", status.inputSampleRate) ||
      !readUint32Field(root, "inputChannelCount",
                       status.inputChannelCount) ||
      !readCounterField(root, "inputFramesReceived",
                        status.inputFramesReceived) ||
      !readCounterField(root, "inputLastReceivedUnixMilliseconds",
                        status.inputLastReceivedUnixMilliseconds)) {
    return responseError("successful control response has invalid status");
  }
  if ((status.processingMode == ProcessingMode::preset &&
       status.activePreset.empty()) ||
      (status.processingMode == ProcessingMode::bypass &&
       !status.activePreset.empty()) ||
      !outputStatusIsConsistent(status)) {
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
  status.defaultSinkActive = yyjson_get_bool(defaultSinkActive);

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
