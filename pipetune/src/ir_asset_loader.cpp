/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "ir_asset_loader.h"
#include "asset_audio_decoder.h"
#include "asset_cache.h"
#include "asset_file.h"
#include "preparation_dispatcher.h"
#include "ir/preparation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <memory>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace pipetune {

static std::string_view jsonText(yyjson_val *value) {
  return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}

static bool hexText(std::string_view value, std::size_t length) {
  return value.size() == length && std::ranges::all_of(value,
      [](char ch) { return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f'); });
}

static double irNumber(yyjson_val *parameters, const char *key, double fallback) {
  auto *const value = yyjson_obj_get(parameters, key);
  if (value == nullptr || yyjson_is_null(value)) return fallback;
  if (!yyjson_is_num(value)) throw std::runtime_error(std::string("IR parameter is not numeric: ") + key);
  return yyjson_get_num(value);
}

static std::uint32_t irHeadBlock(yyjson_val *parameters) {
  auto *const latencyValue = yyjson_obj_get(parameters, "lt");
  const auto latency = jsonText(latencyValue);
  auto head = std::uint32_t{128};
  if (yyjson_is_num(latencyValue)) {
    const auto value = yyjson_get_num(latencyValue);
    if (value != 0 && value != 128 && value != 256 && value != 512 && value != 1024)
      throw std::runtime_error("invalid IR latency");
    head = static_cast<std::uint32_t>(value);
  } else if (!latency.empty()) {
    const auto parsed = std::from_chars(latency.data(), latency.data() + latency.size(), head);
    if (parsed.ec != std::errc{} || parsed.ptr != latency.data() + latency.size())
      throw std::runtime_error("invalid IR latency");
  }
  return head;
}

static assets::IrConfiguration irConfiguration(yyjson_val *parameters, float sampleRate,
                                               std::uint32_t channels, std::uint32_t width) {
  const auto mode = jsonText(yyjson_obj_get(parameters, "cm"));
  const auto rate = jsonText(yyjson_obj_get(parameters, "cr"));
  return assets::resolveIrConfiguration(static_cast<std::uint32_t>(sampleRate), channels, width,
      mode.empty() ? "auto" : mode, irHeadBlock(parameters), rate.empty() ? "auto" : rate);
}

static std::string irCacheIdentity(yyjson_val *parameters, std::string_view id,
    std::span<const std::uint8_t> digests, float sampleRate, std::uint32_t width) {
  // This version identifies the exact app source port and native payload contract.
  // Update it when changing preparation semantics, independently of product versions.
  auto identity = std::ostringstream{};
  identity.imbue(std::locale::classic());
  identity << "effetune:6791afdfc6f4e6c913b9f48c8189a44e29a09bde/ir-v1/ETA1" << '\0'
           << assetDecoderVersion() << '\0' << id << '\0' << assetSha256(digests) << '\0';
  const auto mode = jsonText(yyjson_obj_get(parameters, "cm"));
  const auto rate = jsonText(yyjson_obj_get(parameters, "cr"));
  identity << (mode.empty() ? "auto" : mode) << '\0' << (rate.empty() ? "auto" : rate) << '\0'
           << std::setprecision(17) << irHeadBlock(parameters) << '/' << sampleRate << '/' << width << '/'
           << yyjson_get_bool(yyjson_obj_get(parameters, "dc")) << '/'
           << irNumber(parameters, "co", 0) << '/' << irNumber(parameters, "dt", 100) << '/'
           << irNumber(parameters, "tr", 100);
  const auto bytes = identity.str();
  return assetSha256({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

static cardio::promise<void> verifyIrSources(const std::vector<std::filesystem::path> &files,
                                            const std::vector<std::string> &digests, cardio::cancellation cancellation) {
  // Verify every original and then the registry that selects them. Cached data
  // never substitutes for an available, unchanged source snapshot.
  for (auto index = std::size_t{1}; index <= files.size(); ++index) {
    const auto source = index % files.size();
    const auto bytes = std::move(co_await readAssetFile(files[source], (source == 0 ? 32u : 64u) * 1024u * 1024u, cancellation));
    if (assetSha256(bytes) != digests[source]) throw std::runtime_error("IR source changed during preparation");
  }
}

static cardio::promise<std::shared_ptr<const SharedPreparedAsset>> prepareRegisteredIr(
    const std::filesystem::path &directory, yyjson_val *parameters,
    float sampleRate, std::uint32_t processingChannels,
    const AssetCacheOptions &cache, IrAssetLoadResult &loadResult, SharedPreparedAsset &owned,
    std::string &sharingKey, cardio::cancellation cancellation) {
  co_await preparationCheckpoint(cancellation);
  auto &files = loadResult.files;
  auto &diagnostics = owned.diagnostics;
  const auto id = jsonText(yyjson_obj_get(parameters, "ir"));
  if (id.empty()) {
    diagnostics.emplace_back("No impulse response is assigned; the dry settings remain active");
    co_return nullptr;
  }
  if (!hexText(id, 24)) throw std::runtime_error("IR ID must contain 24 lowercase hexadecimal digits");
  if (directory.empty()) throw std::runtime_error("EffeTune data directory is unavailable");
  const auto library = directory / "ir-library";
  const auto indexPath = library / "index.json";
  files.push_back(indexPath);
  auto indexBytes = std::move(co_await readAssetFile(indexPath, 32u * 1024u * 1024u, cancellation));
  auto index = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(reinterpret_cast<const char *>(indexBytes.data()), indexBytes.size(), 0), yyjson_doc_free);
  auto *root = index != nullptr ? yyjson_doc_get_root(index.get()) : nullptr;
  if (yyjson_get_uint(yyjson_obj_get(root, "version")) != 1)
    throw std::runtime_error("IR library index is invalid or has an unsupported version");
  auto *entry = yyjson_obj_getn(yyjson_obj_get(root, "entries"), id.data(), id.size());
  auto *originals = yyjson_obj_get(entry, "originals");
  const auto composition = jsonText(yyjson_obj_get(entry, "composition"));
  const auto pair = composition == "pair";
  const auto originalCount = pair ? 2u : 1u;
  if (jsonText(yyjson_obj_get(entry, "irId")) != id ||
      (composition != "single" && !pair) || yyjson_arr_size(originals) != originalCount)
    throw std::runtime_error("IR library entry has an invalid composition");
  auto originalsBytes = std::vector<std::vector<std::uint8_t>>{};
  auto sourceDigests = std::vector<std::string>{assetSha256(indexBytes)};
  auto digestBytes = std::array<std::uint8_t, 64>{};
  auto totalBytes = std::uint64_t{0};
  for (auto side = 0u; side < originalCount; ++side) {
    auto *original = yyjson_arr_get(originals, side);
    const auto storageName = jsonText(yyjson_obj_get(original, "storageName"));
    const auto digest = jsonText(yyjson_obj_get(original, "sha256"));
    const auto filename = jsonText(yyjson_obj_get(original, "fileName"));
    auto *bytes = yyjson_obj_get(original, "byteLength");
    const auto role = pair ? (side == 0 ? "L" : "R") : "single";
    const auto base = std::string(id) + (pair ? std::string(".") + role : "");
    auto filenameUnits = std::size_t{0};
    for (const auto character : filename) {
      const auto byte = static_cast<unsigned char>(character);
      if ((byte & 0xc0u) != 0x80u) filenameUnits += byte >= 0xf0u ? 2u : 1u;
    }
    constexpr auto extensions = std::array{".aif", ".aiff", ".bin", ".flac", ".irs", ".m4a", ".mp3", ".ogg", ".wav"};
    if (jsonText(yyjson_obj_get(original, "role")) != role || filename.empty() || filenameUnits > 512 ||
        !storageName.starts_with(base + ".") ||
        std::ranges::find(extensions, storageName.substr(std::min(base.size(), storageName.size()))) == extensions.end() ||
        !hexText(digest, 64) || !yyjson_is_uint(bytes) || yyjson_get_uint(bytes) > 64u * 1024u * 1024u)
      throw std::runtime_error("IR original metadata is invalid");
    totalBytes += yyjson_get_uint(bytes);
    const auto originalPath = library / storageName;
    files.push_back(originalPath);
    auto originalBytes = std::move(co_await readAssetFile(originalPath, 64u * 1024u * 1024u, cancellation));
    const auto actualDigest = assetSha256(originalBytes);
    if (originalBytes.size() != yyjson_get_uint(bytes) || actualDigest != digest ||
        (!pair && actualDigest.substr(0, 24) != id))
      throw std::runtime_error("IR original size, SHA-256, or ID does not match the registry");
    for (auto byte = 0u; byte < 32u; ++byte) {
      auto value = 0u;
      std::from_chars(digest.data() + byte * 2u, digest.data() + byte * 2u + 2u, value, 16);
      digestBytes[side * 32u + byte] = static_cast<std::uint8_t>(value);
    }
    sourceDigests.push_back(actualDigest);
    loadResult.snapshots.push_back({originalPath, actualDigest, originalBytes.size()});
    originalsBytes.push_back(std::move(originalBytes));
  }
  if (!yyjson_is_uint(yyjson_obj_get(entry, "bytes")) ||
      yyjson_get_uint(yyjson_obj_get(entry, "bytes")) != totalBytes)
    throw std::runtime_error("IR library byte count does not match its originals");
  if (pair && assetSha256(digestBytes).substr(0, 24) != id)
    throw std::runtime_error("IR pair ID does not match its originals");
  loadResult.snapshots.push_back({indexPath, sourceDigests.front(), indexBytes.size()});
  const auto key = irCacheIdentity(parameters, id, digestBytes, sampleRate, processingChannels);
  // Registry parsing and the largest PCM analysis workspace do not coexist.
  // Only owned source identities are needed after validating all originals.
  index.reset();
  std::vector<std::uint8_t>().swap(indexBytes);
  sharingKey = directory.lexically_normal().string() + '\0' + cache.directory.lexically_normal().string() +
      '\0' + std::to_string(cache.maximumBytes) + '\0' + key;
  auto &entries = assetPreparationPool().entries;
  std::erase_if(entries, [](const auto &entry) { return entry.second.expired(); });
  if (const auto found = entries.find(sharingKey); found != entries.end()) {
    if (auto reused = found->second.lock()) {
      originalsBytes.clear();
      co_await verifyIrSources(files, sourceDigests, cancellation);
      loadResult.cacheHit = true;
      co_return reused;
    }
  }
  auto cached = std::move(co_await readAssetCache(cache, key, 32u * 1024u * 1024u, cancellation));
  if (cached) {
    originalsBytes.clear();
    co_await verifyIrSources(files, sourceDigests, cancellation);
    loadResult.cacheHit = true;
    owned.cachePin = std::move(cached->pin);
    diagnostics = std::move(cached->diagnostics);
    owned.asset = std::move(cached->asset);
    co_return nullptr;
  }
  auto decoded = std::vector<assets::Audio>{};
  auto configuration = assets::IrConfiguration{};
  for (auto &encoded : originalsBytes) {
    const auto bytes = std::move(encoded);
    co_await preparationCheckpoint(cancellation);
    auto source = decodeAssetAudio(bytes, 64u * 1024u * 1024u);
    if (pair && source.channels.size() != 2)
      throw std::runtime_error("each true-stereo original must contain exactly two channels");
    configuration = irConfiguration(parameters, sampleRate, pair ? 4u : source.channels.size(), processingChannels);
    decoded.push_back(resampleIrAudio(std::move(source), configuration.sampleRate, 64u * 1024u * 1024u));
  }
  auto audio = std::move(decoded.front());
  if (pair) {
    const auto frames = std::max(audio.channels.front().size(), decoded[1].channels.front().size());
    if (frames > 64u * 1024u * 1024u / 4u / sizeof(float))
      throw std::runtime_error("combined true-stereo IR exceeds its PCM budget");
    for (auto &channel : decoded[1].channels) audio.channels.push_back(std::move(channel));
    for (auto &channel : audio.channels) channel.resize(frames, 0.0F);
  }
  co_await preparationCheckpoint(cancellation);
  auto preparation = assets::prepareIr(audio, configuration,
      {.directCut = yyjson_get_bool(yyjson_obj_get(parameters, "dc")),
       .cutOffsetMs = irNumber(parameters, "co", 0),
       .decayPercent = irNumber(parameters, "dt", 100),
       .trimPercent = irNumber(parameters, "tr", 100)});
  if (preparation.capacityLimited)
    diagnostics.emplace_back("Impulse response duration was reduced to fit the convolution memory limit");
  auto prepared = std::move(preparation.asset);
  // Verify the same registry snapshot still selects these sources before persistence.
  co_await verifyIrSources(files, sourceDigests, cancellation);
  auto result = PreparedDspAsset{};
  result.info = {.channels = prepared.channels, .frames = prepared.frames,
      .topology = static_cast<std::uint32_t>(prepared.config.topology),
      .head_block = prepared.config.headBlock, .rate_divider = prepared.config.rateDivider,
      .path_count = static_cast<std::uint32_t>(prepared.config.paths.size()),
      .input_count = prepared.inputCount, .processing_channels = processingChannels,
      .footprint_bytes = static_cast<std::uint32_t>(prepared.footprintBytes),
      .byte_size = static_cast<std::uint32_t>(prepared.payload.size())};
  result.payload = std::move(prepared.payload);
  co_await preparationCheckpoint(cancellation);
  auto saved = std::move(co_await writeAssetCache(cache, key, result, diagnostics, cancellation));
  owned.cachePin = std::move(saved.pin);
  if (!saved.error.empty()) diagnostics.push_back("Prepared impulse response is usable, but its cache could not be saved: " + saved.error);
  co_await verifyIrSources(files, sourceDigests, cancellation);
  owned.asset = std::move(result);
  co_return nullptr;
}

cardio::promise<IrAssetLoadResult> loadIrAssetAsync(
    const std::filesystem::path &directory, yyjson_val *parameters,
    float sampleRate, std::uint32_t processingChannels,
    const AssetCacheOptions &cache, cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes) {
  auto result = IrAssetLoadResult{};
  try {
    auto &pool = assetPreparationPool();
    const auto preparation = std::move(co_await pool.mutex.lock(cancellation));
    const auto assigned = !jsonText(yyjson_obj_get(parameters, "ir")).empty();
    auto memory = AssetMemoryReservation(assigned ? kIrWorkingMemoryBytes : 0, maximumMemoryBytes);
    auto owned = std::make_shared<SharedPreparedAsset>();
    auto key = std::string{};
    result.prepared = std::move(co_await prepareRegisteredIr(directory, parameters, sampleRate, processingChannels,
        cache, result, *owned, key, cancellation));
    if (!result.prepared) {
      // All source, decoder and coefficient temporaries are gone before reducing
      // the reservation to the allocation retained by the immutable result.
      memory.shrink(owned->asset.payload.capacity());
      owned->memory = std::move(memory);
      result.prepared = std::move(owned);
      if (!key.empty()) pool.entries[key] = result.prepared;
    }
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const std::exception &error) {
    result.error = std::string("IR Reverb: ") + error.what();
  }
  co_return result;
}

IrAssetLoadResult loadIrAsset(const std::filesystem::path &directory,
    yyjson_val *parameters, float sampleRate, std::uint32_t processingChannels, const AssetCacheOptions &cache,
    std::uint64_t maximumMemoryBytes) {
  return runPreparation<IrAssetLoadResult>([&] {
    return loadIrAssetAsync(directory, parameters, sampleRate, processingChannels, cache, {}, maximumMemoryBytes);
  });
}

} // namespace pipetune
