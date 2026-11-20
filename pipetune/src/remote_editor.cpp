/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "remote_editor.h"
#include "asset_file.h"
#include "dsp_catalog.h"
#include "pipetune/version.h"
#include "preset-catalog.h"
#include <yyjson.h>
#include <cstdlib>
#include <map>
#include <stdexcept>

namespace pipetune {

using Document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>;
static Document parse(std::string_view json) {
  return Document(yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
}

static std::string encoded(char *buffer) {
  const auto owned = std::unique_ptr<char, decltype(&std::free)>(buffer, std::free);
  if (!owned) throw std::runtime_error("cannot encode JSON");
  return owned.get();
}

static std::string quote(std::string_view text) {
  const auto doc = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  return encoded(yyjson_mut_val_write(yyjson_mut_strn(doc.get(), text.data(), text.size()), 0, nullptr));
}

static std::string_view stringField(yyjson_val *object, const char *key) {
  auto *value = yyjson_obj_get(object, key);
  if (!yyjson_is_str(value)) throw std::invalid_argument(std::string("missing string: ") + key);
  return {yyjson_get_str(value), yyjson_get_len(value)};
}

static void validateChain(yyjson_val *chain) {
  if (!yyjson_is_arr(chain) || yyjson_arr_size(chain) > 256) throw std::invalid_argument("invalid pipeline");
  for (std::size_t index = 0; index < yyjson_arr_size(chain); ++index) {
    auto *stage = yyjson_arr_get(chain, index);
    const auto name = stringField(stage, "nm");
    if (yyjson_obj_get(stage, "name") || !isEditableEffect(name))
      throw std::invalid_argument("unknown effect: " + std::string(name));
  }
}

static std::string mergeParams(std::string_view chain, yyjson_val *request) {
  auto *index = yyjson_obj_get(request, "index");
  auto *params = yyjson_obj_get(request, "params");
  if (!yyjson_is_uint(index) || !yyjson_is_obj(params) || yyjson_obj_get(params, "nm") || yyjson_obj_get(params, "name"))
    throw std::invalid_argument("invalid parameter edit");
  const auto source = parse(chain);
  const auto doc = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_doc_mut_copy(source.get(), nullptr), yyjson_mut_doc_free);
  auto *stage = yyjson_mut_arr_get(yyjson_mut_doc_get_root(doc.get()), yyjson_get_uint(index));
  if (!stage) throw std::invalid_argument("stage index out of range");
  auto it = yyjson_obj_iter_with(params);
  while (auto *key = yyjson_obj_iter_next(&it))
    yyjson_mut_obj_put(stage, yyjson_val_mut_copy(doc.get(), key), yyjson_val_mut_copy(doc.get(), yyjson_obj_iter_get_val(key)));
  return encoded(yyjson_mut_write(doc.get(), 0, nullptr));
}

static bool equalJson(std::string_view left, std::string_view right) {
  if (left == right) return true;
  const auto a = parse(left), b = parse(right);
  return yyjson_equals(yyjson_doc_get_root(a.get()), yyjson_doc_get_root(b.get()));
}

static std::string state(const RemoteEditor &editor, const std::string &seq, bool hello) {
  auto result = std::string{"{\"op\":\"state\",\"rev\":"} + std::to_string(editor.revision) + seq +
      ",\"masterBypass\":" + (editor.previous.bypass ? "true" : "false") +
      ",\"app\":" + quote(version()) + ",\"pipeline\":" + editor.chain;
  if (hello) {
    result += ",\"appName\":\"PipeTune\",\"features\":[],\"effects\":[\"Section\"";
    for (const auto &def : generatedDspCatalog())
      if (isEditableEffect(def.displayName) && def.displayName != "Section") result += "," + quote(def.displayName);
    result += "]";
  }
  return result + "}";
}

std::string RemoteEditor::refresh() {
  auto next = read();
  if (next.recipe == previous.recipe && next.bypass == previous.bypass) return {};
  auto value = next.recipe == previous.recipe ? chain : next.recipe ? shortPipeline(*next.recipe) : "[]";
  const auto changed = next.bypass != previous.bypass || !equalJson(chain, value);
  previous = std::move(next);
  chain = std::move(value);
  if (!changed) return {};
  ++revision;
  lastState = state(*this, {}, false);
  return lastState;
}

static cardio::promise<std::map<std::string, std::string>> availablePresets(cardio::cancellation cancellation) {
  const auto user = pipetune_gtk::resolveEffeTuneUserPresetPath(g_get_user_config_dir(), g_get_home_dir());
  const auto catalog = pipetune_gtk::loadEffeTunePresetCatalog(std::filesystem::path(PIPETUNE_EDITOR_DIRECTORY) / "presets", user.path);
  auto presets = std::map<std::string, std::string>{};
  for (const auto &item : catalog.choices) {
    cancellation.throw_if_cancellation_requested();
    try {
      auto contents = item.serializedPreset;
      if (item.source == pipetune_gtk::PresetSource::standard) {
        const auto bytes = std::move(co_await readAssetFile(item.path, 8 * 1024 * 1024, cancellation));
        contents.assign(bytes.begin(), bytes.end());
      }
      auto pipeline = shortPipeline(contents);
      const auto doc = parse(pipeline);
      validateChain(yyjson_doc_get_root(doc.get()));
      presets.insert_or_assign(item.name, std::move(pipeline));
    } catch (const cardio::canceled_exception &) { throw; }
    catch (const std::exception &) { continue; }
  }
  co_return presets;
}

cardio::promise<RemoteReply> RemoteEditor::request(std::string message, cardio::cancellation cancellation) {
  auto result = RemoteReply{};
  auto seq = std::string{};
  try {
    const auto doc = parse(message);
    auto *root = yyjson_doc_get_root(doc.get());
    auto *number = yyjson_obj_get(root, "seq");
    if (yyjson_is_int(number)) seq = ",\"seq\":" + encoded(yyjson_val_write(number, 0, nullptr));
    const auto op = stringField(root, "op");
    refresh();
    if (op == "hello" || op == "get") {
      if (op == "hello" &&
          (!yyjson_is_uint(yyjson_obj_get(root, "v")) || yyjson_get_uint(yyjson_obj_get(root, "v")) != 1))
        throw std::invalid_argument("unsupported-version");
      result.data = state(*this, seq, op == "hello");
    } else if (op == "chain" || op == "params" || op == "bypass") {
      auto recipe = std::shared_ptr<const std::string>{};
      auto bypass = std::optional<bool>{};
      if (op == "bypass") {
        auto *on = yyjson_obj_get(root, "on");
        if (!yyjson_is_bool(on)) throw std::invalid_argument("bypass requires on");
        bypass = yyjson_get_bool(on);
      } else {
        auto pipeline = std::string{};
        if (op == "chain") {
          auto *value = yyjson_obj_get(root, "pipeline");
          validateChain(value);
          pipeline = encoded(yyjson_val_write(value, 0, nullptr));
        } else {
          pipeline = mergeParams(chain, root);
          const auto values = parse(pipeline);
          validateChain(yyjson_doc_get_root(values.get()));
        }
        recipe = std::make_shared<const std::string>(std::move(pipeline));
      }
      const auto error = std::move(co_await apply(std::move(recipe), bypass, cancellation));
      if (!error.empty()) throw std::invalid_argument(error);
      refresh();
    } else if (op == "listPresets" || op == "getPreset") {
      const auto presets = std::move(co_await availablePresets(cancellation));
      if (op == "listPresets") {
        auto names = std::string{};
        for (const auto &[name, value] : presets) {
          if (!names.empty()) names += ",";
          names += quote(name);
        }
        result.data = "{\"op\":\"presets\",\"names\":[" + names + "]" + seq + "}";
      } else {
        const auto name = std::string(stringField(root, "name"));
        const auto it = presets.find(name);
        if (it == presets.end()) throw std::invalid_argument("unknown preset");
        result.data = "{\"op\":\"preset\",\"name\":" + quote(name) + ",\"pipeline\":" + it->second + seq + "}";
      }
    } else throw std::invalid_argument("unsupported-op");
    if (!seq.empty()) result.ack = "{\"op\":\"ack\",\"ok\":true,\"rev\":" + std::to_string(revision) + seq + "}";
  } catch (const std::exception &error) {
    result.ack = "{\"op\":\"ack\",\"ok\":false,\"error\":" + quote(error.what()) + seq + "}";
  }
  co_return result;
}
}
