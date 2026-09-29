/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "measurement_store.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <fstream>
#include <set>

namespace pipetune {

std::filesystem::path resolveEffeTuneDirectory(
    std::string_view xdgConfigHome, const std::filesystem::path &homeDirectory) {
  if (!xdgConfigHome.empty()) return std::filesystem::path(xdgConfigHome) / "effetune";
  return homeDirectory.empty() ? std::filesystem::path{} : homeDirectory / ".config" / "effetune";
}

static std::string_view text(yyjson_val *value) {
  return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}

static bool validId(std::string_view id) {
  if (id.empty() || id.size() > 120u || id.find("..") != id.npos || id.front() == '.') return false;
  return std::ranges::all_of(id, [](char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
  });
}

static yyjson_doc *readSession(MeasurementStore &store, const std::string &id) {
  const auto found = store.documents.find(id);
  if (found != store.documents.end()) return found->second.get();
  auto &document = store.documents[id];
  if (store.directory.empty()) return nullptr;
  const auto path = store.directory / (id + ".json");
  auto error = std::error_code{};
  const auto size = std::filesystem::file_size(path, error);
  if (error || size == 0 || size > 64u * 1024u * 1024u) return nullptr;
  auto file = std::ifstream(path, std::ios::binary);
  auto contents = std::string(static_cast<std::size_t>(size), '\0');
  if (!file.read(contents.data(), static_cast<std::streamsize>(size))) return nullptr;
  document = std::shared_ptr<yyjson_doc>(yyjson_read(contents.data(), contents.size(), 0), yyjson_doc_free);
  return document.get();
}

static int base64Digit(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  return c == '+' ? 62 : c == '/' ? 63 : -1;
}

static bool decodeSamples(std::string_view encoded, std::vector<float> &samples) {
  if (encoded.empty() || encoded.size() % 4 != 0) return false;
  auto bytes = std::vector<std::uint8_t>{};
  bytes.reserve(encoded.size() / 4u * 3u);
  for (std::size_t i = 0; i < encoded.size(); i += 4) {
    const auto a = base64Digit(encoded[i]), b = base64Digit(encoded[i + 1]);
    const auto c = encoded[i + 2] == '=' ? 0 : base64Digit(encoded[i + 2]);
    const auto d = encoded[i + 3] == '=' ? 0 : base64Digit(encoded[i + 3]);
    const auto padding = encoded[i + 2] == '=' ? 2 : encoded[i + 3] == '=' ? 1 : 0;
    if (a < 0 || b < 0 || c < 0 || d < 0 ||
        (padding && i + 4 != encoded.size()) ||
        (padding == 2 && (encoded[i + 3] != '=' || (b & 15))) ||
        (padding == 1 && (c & 3))) return false;
    const auto value = static_cast<std::uint32_t>((a << 18) | (b << 12) | (c << 6) | d);
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    if (padding < 2) bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    if (padding == 0) bytes.push_back(static_cast<std::uint8_t>(value));
  }
  if (bytes.empty() || bytes.size() % 4u != 0) return false;
  samples.reserve(bytes.size() / 4u);
  for (std::size_t i = 0; i < bytes.size(); i += 4) {
    const auto bits = std::uint32_t{bytes[i]} | (std::uint32_t{bytes[i + 1]} << 8) |
        (std::uint32_t{bytes[i + 2]} << 16) | (std::uint32_t{bytes[i + 3]} << 24);
    const auto value = std::bit_cast<float>(bits);
    if (!std::isfinite(value)) return false;
    samples.push_back(value);
  }
  return true;
}

static bool safeInteger(yyjson_val *value) {
  const auto number = yyjson_get_num(value);
  return yyjson_is_num(value) && std::isfinite(number) &&
      std::trunc(number) == number && std::abs(number) <= 9007199254740991.0;
}

static std::string extractImpulse(yyjson_val *root, std::string_view id,
                                  std::string_view channel, MeasuredImpulse &out) {
  if (!yyjson_is_obj(root) || text(yyjson_obj_get(root, "id")) != id) return "measurement ID does not match its file";
  auto *points = yyjson_obj_get(root, "points");
  if (yyjson_arr_size(points) != 1u) return "measurement must contain exactly one point";
  auto *point = yyjson_arr_get(points, 0);
  auto *key = yyjson_obj_get(point, "pointId");
  auto *outputs = yyjson_obj_get(root, "outputChannels");
  if (yyjson_arr_size(outputs) > 1) {
    bool declared = false;
    for (std::size_t i = 0; i < yyjson_arr_size(outputs); ++i) declared |= text(yyjson_arr_get(outputs, i)) == channel;
    if (!declared) return "measurement channel is unavailable";
    key = nullptr;
    auto *channels = yyjson_obj_get(point, "channels");
    for (std::size_t i = 0; i < yyjson_arr_size(channels); ++i) {
      auto *entry = yyjson_arr_get(channels, i);
      if (text(yyjson_obj_get(entry, "channel")) == channel &&
          yyjson_is_true(yyjson_obj_get(yyjson_obj_get(entry, "ir"), "stored"))) key = yyjson_obj_get(entry, "irId");
    }
  } else if (!channel.empty()) {
    return "measurement is not multichannel";
  }
  if (!safeInteger(key)) return "stored impulse response reference is missing";
  auto *records = yyjson_obj_get(root, "impulseResponses");
  yyjson_val *selected = nullptr;
  for (std::size_t i = 0; i < yyjson_arr_size(records); ++i) {
    auto *record = yyjson_arr_get(records, i);
    auto *recordKey = yyjson_obj_get(record, "pointId");
    if (safeInteger(recordKey) && yyjson_get_num(recordKey) == yyjson_get_num(key)) {
      if (selected != nullptr) return "duplicate impulse response records";
      selected = record;
    }
  }
  if (selected == nullptr || text(yyjson_obj_get(selected, "measurementId")) != id ||
      (!channel.empty() && text(yyjson_obj_get(selected, "channel")) != channel)) return "impulse response is missing or belongs to another channel";
  const auto reference = text(yyjson_obj_get(selected, "outputTimeReference"));
  if (reference != "audio-context" && reference != "file") return "unsupported output time reference; measure again";
  auto *rate = yyjson_obj_get(selected, "sampleRate");
  auto *trim = yyjson_obj_get(selected, "trimStartSamples");
  auto *onset = yyjson_obj_get(selected, "onsetIndex");
  if (!safeInteger(rate) || yyjson_get_num(rate) <= 0 || !safeInteger(trim) || !safeInteger(onset) || yyjson_get_num(onset) < 0) return "invalid measurement timing metadata";
  out.sampleRate = yyjson_get_num(rate);
  out.trimStartSamples = static_cast<std::int64_t>(yyjson_get_num(trim));
  out.onsetIndex = static_cast<std::int64_t>(yyjson_get_num(onset));
  const auto scale = yyjson_get_num(yyjson_obj_get(selected, "refScale"));
  out.referenceScale = std::isfinite(scale) && scale > 1e-12 ? scale : 1;
  if (!decodeSamples(text(yyjson_obj_get(selected, "data")), out.samples) ||
      static_cast<std::uint64_t>(out.onsetIndex) >= out.samples.size()) return "invalid impulse response samples or onset";
  return {};
}

CrosstalkMeasurements loadCrosstalkMeasurements(
    MeasurementStore &store, const std::array<std::string, 4> &ids) {
  auto result = CrosstalkMeasurements{};
  auto baseIds = std::array<std::string, 4>{};
  auto channels = std::array<std::string, 4>{};
  // Collect every valid dependency before reading, including missing files.
  for (std::size_t i = 0; i < ids.size(); ++i) {
    const auto separator = ids[i].rfind("::ch=");
    baseIds[i] = ids[i].substr(0, separator);
    if (separator != std::string::npos) channels[i] = ids[i].substr(separator + 5);
    if (!validId(baseIds[i])) { result.error = "invalid or unassigned measurement ID"; continue; }
    if (!store.directory.empty()) {
      const auto path = store.directory / (baseIds[i] + ".json");
      if (std::ranges::find(result.files, path) == result.files.end()) result.files.push_back(path);
    }
  }
  if (!result.error.empty()) return result;
  if (store.directory.empty()) { result.error = "EffeTune measurement directory is unavailable"; return result; }
  if (std::set<std::string>(ids.begin(), ids.end()).size() != 4u || baseIds[0] != baseIds[2] || baseIds[1] != baseIds[3]) {
    result.error = "assign distinct channels of one measurement for each ear"; return result;
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    auto *document = readSession(store, baseIds[i]);
    if (document == nullptr) result.error = "cannot read measurement JSON (missing, invalid, or larger than 64 MiB)";
    else result.error = extractImpulse(yyjson_doc_get_root(document), baseIds[i], channels[i], result.sources[i]);
    if (!result.error.empty()) { result.error = ids[i] + ": " + result.error; return result; }
    if (i != 0 && result.sources[i].sampleRate != result.sources[0].sampleRate) {
      result.error = "all four measurements must use the same sample rate"; return result;
    }
  }
  return result;
}
} // namespace pipetune
