#include "control_protocol.h"

#include <yyjson.h>

#include <cstdlib>
#include <cstring>
#include <memory>
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
  return {.request = {.command = ControlCommand::status, .presetPath = {}},
          .error = std::move(message)};
}

static bool addString(yyjson_mut_doc *document, yyjson_mut_val *object,
                      const char *key, std::string_view value) {
  return yyjson_mut_obj_add_strncpy(document, object, key, value.data(),
                                    value.size());
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
    return {.request = {.command = ControlCommand::status, .presetPath = {}},
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
                      .presetPath = std::move(preset)},
          .error = {}};
}

std::string makeStatusControlRequest() {
  return R"json({"command":"status"})json";
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

std::string makeControlSuccessResponse(
    const ControlRuntimeStatus &status,
    std::span<const PipelineWarning> warnings) {
  yyjson_mut_val *root = nullptr;
  auto document = createObjectDocument(root);
  if (document == nullptr ||
      !yyjson_mut_obj_add_bool(document.get(), root, "ok", true) ||
      !addString(document.get(), root, "preset", status.activePreset) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "activePluginCount",
                               status.activePluginCount) ||
      !addString(document.get(), root, "selectedTarget",
                 status.selectedTarget) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "overrunFrames",
                               status.overrunFrames) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "underrunFrames",
                               status.underrunFrames) ||
      !yyjson_mut_obj_add_uint(document.get(), root, "processingErrors",
                               status.processingErrors)) {
    return makeControlErrorResponse("cannot encode control response");
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
