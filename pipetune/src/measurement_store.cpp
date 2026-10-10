/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "measurement_store.h"
#include "preparation_dispatcher.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <set>
#include <stdexcept>

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

MeasurementReference parseMeasurementReference(std::string_view reference) {
  const auto separator = reference.rfind("::ch=");
  auto result = MeasurementReference{std::string(reference.substr(0, separator)), {}};
  if (separator != reference.npos) result.channel = reference.substr(separator + 5);
  if (!validId(result.id) || (separator != reference.npos && result.channel.empty()) ||
      result.channel.find('\0') != result.channel.npos)
    throw std::invalid_argument("invalid or unassigned measurement ID");
  return result;
}

cardio::promise<yyjson_doc *> readMeasurementSessionAsync(MeasurementStore &store,
    const std::string &id, std::size_t maximumBytes, cardio::cancellation cancellation) {
  if (!validId(id)) throw std::invalid_argument("invalid measurement ID");
  const auto found = store.documents.find(id);
  if (found != store.documents.end()) co_return found->second.get();
  if (store.directory.empty()) throw std::runtime_error("EffeTune measurement directory is unavailable");
  const auto path = store.directory / (id + ".json");
  const auto bytes = std::move(co_await readAssetFile(path, std::min<std::size_t>(maximumBytes, 64u * 1024u * 1024u), cancellation));
  auto document = std::shared_ptr<yyjson_doc>(yyjson_read(reinterpret_cast<const char *>(bytes.data()), bytes.size(), 0), yyjson_doc_free);
  if (!document) throw std::runtime_error("cannot parse measurement JSON");
  auto *root = yyjson_doc_get_root(document.get());
  if (!yyjson_is_obj(root) || text(yyjson_obj_get(root, "id")) != id)
    throw std::runtime_error("measurement ID does not match its file");
  store.snapshots.insert_or_assign(id, AssetFileSnapshot{path, assetSha256(bytes), bytes.size()});
  auto *result = document.get();
  store.documents.emplace(id, std::move(document));
  co_return result;
}

std::vector<std::pair<double, double>> measurementFrequencyResponse(
    yyjson_val *root, std::string_view channel) {
  auto *response = yyjson_obj_get(root, "averageFrequencyResponse");
  const auto multichannel = yyjson_arr_size(yyjson_obj_get(root, "outputChannels")) > 1;
  if (multichannel) {
    auto declared = false;
    auto *channels = yyjson_obj_get(root, "outputChannels");
    for (auto index = std::size_t{0}; index < yyjson_arr_size(channels); ++index)
      declared = declared || text(yyjson_arr_get(channels, index)) == channel;
    if (channel.empty() || !declared) throw std::invalid_argument("measurement virtual channel is unavailable");
    response = nullptr;
    auto *summaries = yyjson_obj_get(root, "channelResponses");
    for (auto index = std::size_t{0}; index < yyjson_arr_size(summaries); ++index) {
      auto *entry = yyjson_arr_get(summaries, index);
      if (text(yyjson_obj_get(entry, "channel")) == channel) {
        response = yyjson_obj_get(entry, "averageFrequencyResponse"); break;
      }
    }
  } else if (!channel.empty()) throw std::invalid_argument("measurement is not multichannel");
  if (!yyjson_is_arr(response) || yyjson_arr_size(response) == 0)
    throw std::invalid_argument("measurement frequency response is unavailable");
  auto result = std::vector<std::pair<double, double>>{};
  result.reserve(yyjson_arr_size(response));
  for (auto index = std::size_t{0}; index < yyjson_arr_size(response); ++index) {
    auto *point = yyjson_arr_get(response, index);
    auto *frequency = yyjson_is_arr(point) ? yyjson_arr_get(point, 0) : yyjson_obj_get(point, "frequency");
    auto *magnitude = yyjson_is_arr(point) ? yyjson_arr_get(point, 1) : yyjson_obj_get(point, "magnitude");
    if (!yyjson_is_num(frequency) || !yyjson_is_num(magnitude) ||
        !std::isfinite(yyjson_get_num(frequency)) || yyjson_get_num(frequency) <= 0 ||
        !std::isfinite(yyjson_get_num(magnitude)))
      throw std::invalid_argument("invalid measurement frequency response");
    result.emplace_back(yyjson_get_num(frequency), yyjson_get_num(magnitude));
  }
  return result;
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

std::vector<MeasuredPointImpulse> measurementImpulseResponses(yyjson_val *root, std::string_view channel) {
  auto *points = yyjson_obj_get(root, "points");
  auto *records = yyjson_obj_get(root, "impulseResponses");
  const auto id = text(yyjson_obj_get(root, "id"));
  auto selected = std::vector<yyjson_val *>{};
  auto result = std::vector<MeasuredPointImpulse>{};
  auto pointIds = std::set<std::uint64_t>{};
  auto projectedRecordIds = std::set<double>{};
  for (auto index = std::size_t{0}; index < yyjson_arr_size(points); ++index) {
    auto *point = yyjson_arr_get(points, index);
    auto *pointId = yyjson_obj_get(point, "pointId");
    if (!safeInteger(pointId) || yyjson_get_num(pointId) < 0)
      throw std::invalid_argument("invalid measurement impulse point ID");
    if (!pointIds.insert(static_cast<std::uint64_t>(yyjson_get_num(pointId))).second)
      throw std::invalid_argument("duplicate measurement impulse point ID");
    auto *recordId = pointId;
    if (!channel.empty()) {
      recordId = nullptr;
      auto *channels = yyjson_obj_get(point, "channels");
      for (auto slot = std::size_t{0}; slot < yyjson_arr_size(channels); ++slot) {
        auto *entry = yyjson_arr_get(channels, slot);
        if (text(yyjson_obj_get(entry, "channel")) == channel) {
          recordId = yyjson_obj_get(entry, "irId"); break;
        }
      }
    }
    if (!safeInteger(recordId)) return {};
    // The application's irId-to-point map retains the last point. A duplicate
    // therefore leaves an earlier point without an IR, so the set is incomplete.
    if (!channel.empty() && !projectedRecordIds.insert(yyjson_get_num(recordId)).second) return {};
    yyjson_val *matched = nullptr;
    for (auto slot = std::size_t{0}; slot < yyjson_arr_size(records); ++slot) {
      auto *record = yyjson_arr_get(records, slot);
      auto *key = yyjson_obj_get(record, "pointId");
      if (safeInteger(key) && yyjson_get_num(key) == yyjson_get_num(recordId) &&
          text(yyjson_obj_get(record, "measurementId")) == id &&
          (channel.empty() || text(yyjson_obj_get(record, "channel")) == channel)) matched = record;
    }
    if (!matched || text(yyjson_obj_get(matched, "data")).empty()) return {};
    selected.push_back(matched);
    result.push_back({static_cast<std::uint64_t>(yyjson_get_num(pointId)), {}});
  }
  for (auto index = std::size_t{0}; index < selected.size(); ++index) {
    auto *record = selected[index];
    auto *rate = yyjson_obj_get(record, "sampleRate");
    auto *onset = yyjson_obj_get(record, "onsetIndex");
    if (!safeInteger(rate) || yyjson_get_num(rate) <= 0 || yyjson_get_num(rate) > 768000 ||
        !safeInteger(onset) || yyjson_get_num(onset) < 0)
      throw std::invalid_argument("invalid measurement impulse timing metadata");
    auto &impulse = result[index].impulse;
    impulse.sampleRate = yyjson_get_num(rate);
    impulse.onsetIndex = static_cast<std::int64_t>(yyjson_get_num(onset));
    auto *trim = yyjson_obj_get(record, "trimStartSamples");
    if (safeInteger(trim)) impulse.trimStartSamples = static_cast<std::int64_t>(yyjson_get_num(trim));
    auto *scale = yyjson_obj_get(record, "refScale");
    impulse.referenceScale = yyjson_is_num(scale) && std::isfinite(yyjson_get_num(scale)) ? yyjson_get_num(scale) : 1;
    if (!decodeSamples(text(yyjson_obj_get(record, "data")), impulse.samples) ||
        static_cast<std::uint64_t>(impulse.onsetIndex) >= impulse.samples.size())
      throw std::invalid_argument("invalid measurement impulse samples or onset");
  }
  return result;
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

cardio::promise<CrosstalkMeasurements> loadCrosstalkMeasurementsAsync(
    MeasurementStore &store, const std::array<std::string, 4> &ids, cardio::cancellation cancellation) {
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
  if (!result.error.empty()) co_return result;
  if (store.directory.empty()) { result.error = "EffeTune measurement directory is unavailable"; co_return result; }
  if (std::set<std::string>(ids.begin(), ids.end()).size() != 4u || baseIds[0] != baseIds[2] || baseIds[1] != baseIds[3]) {
    result.error = "assign distinct channels of one measurement for each ear"; co_return result;
  }
  for (std::size_t i = 0; i < ids.size(); ++i) {
    try {
      auto *document = co_await readMeasurementSessionAsync(store, baseIds[i], 64u * 1024u * 1024u, cancellation);
      result.error = extractImpulse(yyjson_doc_get_root(document), baseIds[i], channels[i], result.sources[i]);
    } catch (const cardio::canceled_exception &) { throw; }
    catch (const std::exception &error) { result.error = error.what(); }
    if (!result.error.empty()) { result.error = ids[i] + ": " + result.error; co_return result; }
    if (i != 0 && result.sources[i].sampleRate != result.sources[0].sampleRate) {
      result.error = "all four measurements must use the same sample rate"; co_return result;
    }
  }
  co_return result;
}

CrosstalkMeasurements loadCrosstalkMeasurements(
    MeasurementStore &store, const std::array<std::string, 4> &ids) {
  return runPreparation<CrosstalkMeasurements>([&] { return loadCrosstalkMeasurementsAsync(store, ids, {}); });
}
} // namespace pipetune
