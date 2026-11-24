/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "speaker_correction_asset.h"
#include "measurement_store.h"

#include <algorithm>
#include <bit>
#include <cmath>

namespace pipetune {

static constexpr std::uint32_t channelCount = 16;
static constexpr std::uint32_t evenWeights = 1182;
static constexpr std::uint32_t oddWeights = 32;
static constexpr std::uint32_t frames = evenWeights + 1 + 2 * oddWeights;
static constexpr std::uint32_t headerBytes = 32;
static constexpr std::uint32_t payloadBytes = headerBytes + channelCount * frames * sizeof(float);

static std::string_view text(yyjson_val *value) {
  return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}

static void writeWord(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
  for (auto i = 0u; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> (8 * i));
}

// The backup stores typed arrays as { "f32": "<base64>" }. Only the coefficients
// used for playback need decoding; the measurement also contains its original fitting data.
static void writeCoefficients(std::span<std::uint8_t> dest, yyjson_val *field,
                              std::size_t count, bool bounds) {
  const auto encoded = text(yyjson_obj_get(field, "f32"));
  auto values = std::vector<float>{};
  if (encoded.size() != 4 * ((sizeof(float) * count + 2) / 3) ||
      !decodeMeasurementSamples(encoded, values) || values.size() != count)
    throw std::invalid_argument("invalid speaker model coefficient array");
  for (auto i = std::size_t{0}; i < count; ++i) {
    const auto value = values[i];
    if (bounds ? value <= 0 : std::abs(value) >= 1000)
      throw std::invalid_argument("speaker model coefficient out of range");
    writeWord(dest, i * sizeof(float), std::bit_cast<std::uint32_t>(value));
  }
}

static std::uint32_t channelMask(yyjson_val *parameters, const char *key) {
  const auto number = yyjson_get_num(yyjson_obj_get(parameters, key));
  return number >= 0 && number <= 65535 && std::trunc(number) == number ?
      static_cast<std::uint32_t>(number) : 0u;
}

static PreparedDspAsset packSpeakerModel(yyjson_val *model, yyjson_val *parameters,
                                        std::uint32_t outputChannels) {
  auto *channels = yyjson_obj_get(model, "channels");
  if (!yyjson_arr_size(channels) || yyjson_arr_size(channels) > channelCount)
    throw std::invalid_argument("invalid speaker model channels");
  auto result = PreparedDspAsset{};
  result.payload.resize(payloadBytes);
  result.info = {.channels = channelCount, .frames = frames, .topology = 2, .head_block = 0,
      .rate_divider = 1, .path_count = 0, .input_count = 0, .processing_channels = outputChannels,
      .footprint_bytes = payloadBytes, .byte_size = payloadBytes};
  writeWord(result.payload, 0, 0x31415445u);
  writeWord(result.payload, 4, channelCount);
  writeWord(result.payload, 8, frames);
  writeWord(result.payload, 16, result.info.topology);
  const auto isLong = text(yyjson_obj_get(parameters, "fl")) == "Long";
  const auto *length = isLong ? "long" : "short";
  if (isLong) {
    for (auto i = 0u; i < channelCount; ++i)
      writeWord(result.payload, headerBytes + (i * frames + evenWeights) * sizeof(float),
                std::bit_cast<std::uint32_t>(1.0F));
  }
  const auto evenOn = channelMask(parameters, "cn"), evenOff = channelMask(parameters, "co");
  const auto oddOn = channelMask(parameters, "cn3"), oddOff = channelMask(parameters, "co3");
  auto used = std::uint32_t{0};
  for (auto i = std::size_t{0}; i < yyjson_arr_size(channels); ++i) {
    auto *entry = yyjson_arr_get(channels, i);
    auto *index = yyjson_obj_get(entry, "index");
    if (!yyjson_is_uint(index) || yyjson_get_uint(index) >= channelCount)
      throw std::invalid_argument("invalid physical speaker index");
    const auto physical = yyjson_get_uint(index);
    const auto bit = 1u << physical;
    if (used & bit) throw std::invalid_argument("duplicate physical speaker index");
    used |= bit;
    if (physical >= outputChannels) continue;

    auto data = std::span(result.payload).subspan(headerBytes + physical * frames * sizeof(float),
                                                   frames * sizeof(float));
    auto *weights = yyjson_obj_get(yyjson_obj_get(entry, "weights"), length);
    const auto reason = text(yyjson_obj_get(yyjson_obj_get(entry, "reason"), length));
    const auto even = weights && !yyjson_is_null(weights) && reason != "belowLimit" && reason != "noFit" &&
        !(evenOff & bit) && ((evenOn & bit) || yyjson_is_true(yyjson_obj_get(yyjson_obj_get(entry, "on"), length)));
    if (even) writeCoefficients(data, weights, evenWeights, false);

    auto *odd = yyjson_obj_get(entry, "odd");
    auto *oddFit = yyjson_obj_get(odd, "weights");
    const auto automatic = text(yyjson_obj_get(odd, "fitReason")).empty() &&
        yyjson_is_true(yyjson_obj_get(yyjson_obj_get(odd, "on"), even ? length : "alone"));
    if (oddFit && !yyjson_is_null(oddFit) && !(oddOff & bit) && ((oddOn & bit) || automatic)) {
      writeCoefficients(data.subspan((evenWeights + 1) * sizeof(float)), oddFit, oddWeights, false);
      writeCoefficients(data.subspan((evenWeights + 1 + oddWeights) * sizeof(float)),
                        yyjson_obj_get(odd, "bounds"), oddWeights, true);
    }
  }
  return result;
}

cardio::promise<SpeakerCorrectionLoadResult> loadSpeakerCorrectionAsync(
    const std::filesystem::path &directory, yyjson_val *parameters, std::uint32_t outputChannels,
    AssetMemoryReservation &memory, std::uint64_t maximumMemoryBytes, cardio::cancellation cancellation) {
  auto result = SpeakerCorrectionLoadResult{};
  const auto id = text(yyjson_obj_get(parameters, "ms"));
  if (id.empty()) co_return result;
  try {
    const auto reference = parseMeasurementReference(id);
    if (!reference.channel.empty()) throw std::invalid_argument("expected a physical measurement ID");
    if (directory.empty()) throw std::runtime_error("measurement directory is unavailable");
    result.file = directory / (reference.id + ".json");
    memory.resize(128 * 1024, maximumMemoryBytes);
    auto inspected = std::move(co_await inspectAssetFile(result.file, 64u * 1024 * 1024, 0, cancellation));
    const auto bytes = inspected.snapshot.bytes;
    memory.resize(256u * 1024 + bytes + yyjson_read_max_memory_usage(bytes, 0), maximumMemoryBytes);
    auto store = MeasurementStore{directory, {}};
    auto *document = co_await readMeasurementSessionAsync(store, reference.id, bytes, cancellation);
    if (store.snapshots.at(reference.id).digest != inspected.snapshot.digest)
      throw std::runtime_error("measurement changed during preparation");
    result.snapshot = std::move(inspected.snapshot);
    auto *model = yyjson_obj_get(yyjson_doc_get_root(document), "speakerModel");
    result.asset = packSpeakerModel(model, parameters, outputChannels);
    auto *check = yyjson_obj_get(model, "lastCheck");
    const auto outcome = text(yyjson_obj_get(check, "outcome"));
    auto *volume = yyjson_obj_get(check, "dv");
    if (yyjson_get_num(yyjson_obj_get(check, "at")) > yyjson_get_num(yyjson_obj_get(parameters, "dc")) &&
        (outcome == "lower" || outcome == "same" || outcome == "louder") && yyjson_is_num(volume))
      result.checkedVolume = static_cast<float>(std::clamp(yyjson_get_num(volume), -24.0, 0.0));
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const std::exception &error) {
    result.asset.error = "Distortion Correction measurement " + std::string(id) + ": " + error.what();
  }
  co_return result;
}

} // namespace pipetune
