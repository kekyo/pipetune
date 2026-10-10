/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "room_asset_loader.h"
#include "asset_cache.h"
#include "measurement_store.h"
#include "preparation_dispatcher.h"
#include "common/convolution.h"
#include "room-eq/design.h"

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <iomanip>
#include <locale>
#include <sstream>
#include <stdexcept>

namespace pipetune {

static std::string_view jsonText(yyjson_val *value) {
  return yyjson_is_str(value) ? std::string_view(yyjson_get_str(value), yyjson_get_len(value)) : std::string_view{};
}

static double roomNumber(yyjson_val *parameters, const char *key, double fallback, double low, double high) {
  auto *value = yyjson_obj_get(parameters, key);
  if (!value || yyjson_is_null(value)) return fallback;
  auto number = yyjson_get_num(value);
  if (yyjson_is_str(value)) {
    const auto text = jsonText(value);
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
      throw std::invalid_argument(std::string("invalid Room EQ numeric setting: ") + key);
  } else if (!yyjson_is_num(value)) throw std::invalid_argument(std::string("invalid Room EQ numeric setting: ") + key);
  if (!std::isfinite(number)) throw std::invalid_argument(std::string("nonfinite Room EQ setting: ") + key);
  return std::clamp(number, low, high);
}

static bool roomBoolean(yyjson_val *parameters, const char *key, bool fallback) {
  auto *const value = yyjson_obj_get(parameters, key);
  if (!value) return fallback;
  return yyjson_is_true(value) || (yyjson_is_num(value) && yyjson_get_num(value) == 1) ||
      (yyjson_is_str(value) && (std::string_view(yyjson_get_str(value)) == "true" || std::string_view(yyjson_get_str(value)) == "1"));
}

static assets::RoomEqConfiguration roomConfiguration(yyjson_val *parameters, float sampleRate) {
  auto result = assets::RoomEqConfiguration{};
  result.sampleRate = static_cast<std::uint32_t>(sampleRate);
  const auto phase = jsonText(yyjson_obj_get(parameters, "pm"));
  if (phase.empty() || phase == "min") result.phase = assets::RoomEqPhase::Minimum;
  else if (phase == "lin") result.phase = assets::RoomEqPhase::Linear;
  else if (phase == "full") result.phase = assets::RoomEqPhase::Correction;
  else throw std::invalid_argument("invalid Room EQ phase mode");
  const auto taps = roomNumber(parameters, "tp", 32768, 0, UINT32_MAX);
  if (taps >= 8192 && taps <= 131072 && std::trunc(taps) == taps &&
      std::has_single_bit(static_cast<std::uint32_t>(taps))) result.taps = static_cast<std::uint32_t>(taps);
  result.smoothing = roomNumber(parameters, "sm", 0.17, 0.02, 1);
  result.lowFrequency = roomNumber(parameters, "fl", 80, 20, 1000);
  result.highFrequency = roomNumber(parameters, "fh", 16000, 1000, 20000);
  result.maxBoostDb = roomNumber(parameters, "mb", 6, 0, 18);
  result.correctionAmount = std::floor(roomNumber(parameters, "cr", 100, 0, 100) + 0.5) / 100;
  result.directWindowMs = roomNumber(parameters, "dw", 6, 1, 50);
  if (!roomBoolean(parameters, "pa", true)) {
    const auto automatic = std::round(std::max(result.lowFrequency, 3000 / result.directWindowMs));
    result.phaseLowFrequency = std::max(std::ceil(1000 / result.directWindowMs),
        std::round(roomNumber(parameters, "pl", automatic, 20, 20000)));
  }
  result.phaseCorrectionAmount = std::round(roomNumber(parameters, "pr", 100, 0, 100)) / 100;
  result.reverbAmount = std::round(roomNumber(parameters, "rv", 0, 0, 100)) / 100;
  result.reverbWindowMs = roomNumber(parameters, "rw", 300, 20, 1000);
  result.reverbMaxFrequency = std::round(roomNumber(parameters, "rf", 250, 20, 20000));
  result.reverbSmoothing = roomNumber(parameters, "rs", 0.05, 0.02, 1);
  if (!roomBoolean(parameters, "pq", true)) result.phaseSmoothing = roomNumber(parameters, "ps", result.smoothing, 0.02, 1);
  result.lowFrequencyPhaseExtension = result.phase == assets::RoomEqPhase::Correction && roomBoolean(parameters, "le", false);
  const auto point = roomNumber(parameters, "rp", 0, 0, 9007199254740991.0);
  if (point == std::trunc(point)) result.referencePoint = static_cast<std::uint64_t>(point);
  auto *bands = yyjson_obj_get(parameters, "bs");
  if (bands && !yyjson_is_arr(bands)) throw std::invalid_argument("Room EQ additional EQ must be an array");
  constexpr auto frequencies = std::array{100, 316, 1000, 3160, 10000};
  for (auto index = std::size_t{0}; index < std::min(yyjson_arr_size(bands), frequencies.size()); ++index) {
    auto *entry = yyjson_arr_get(bands, index);
    if (yyjson_is_null(entry)) continue;
    if (!yyjson_is_obj(entry)) throw std::invalid_argument("invalid Room EQ additional EQ band");
    auto band = assets::RoomEqBand{};
    band.frequency = roomNumber(entry, "frequency", frequencies[index], 20, 20000);
    band.gain = roomNumber(entry, "gain", 0, -20, 20);
    band.q = roomNumber(entry, "q", 1, 0.1, 10);
    auto *enabled = yyjson_obj_get(entry, "enabled");
    band.enabled = !enabled || yyjson_is_true(enabled) || (yyjson_is_num(enabled) && yyjson_get_num(enabled) != 0);
    const auto type = jsonText(yyjson_obj_get(entry, "type"));
    band.type = type == "ls" ? assets::RoomEqBandType::LowShelf :
                type == "hs" ? assets::RoomEqBandType::HighShelf : assets::RoomEqBandType::Peak;
    result.eqBands.push_back(band);
  }
  return result;
}

static void appendRoomDiagnostic(SharedPreparedAsset &owned, std::string_view label,
    const assets::RoomEqDiagnostic &diagnostic, bool showApplied) {
  if (diagnostic.reason == "notRequested" || diagnostic.reason == "phaseCorrectionDisabled" ||
      diagnostic.state == "notRequested" || (diagnostic.state == "applied" && !showApplied)) return;
  auto message = std::ostringstream{};
  message << std::setprecision(10) << "Room EQ " << label << ": " << diagnostic.state;
  if (diagnostic.reason) message << "; reason=" << *diagnostic.reason;
  if (diagnostic.scale) message << "; scale=" << *diagnostic.scale;
  if (diagnostic.effectiveWindowMs) message << "; effectiveWindowMs=" << *diagnostic.effectiveWindowMs;
  if (diagnostic.agreementMinimum) message << "; agreementMinimum=" << *diagnostic.agreementMinimum;
  if (diagnostic.residualMaximumMs) message << "; residualMaximumMs=" << *diagnostic.residualMaximumMs;
  owned.diagnostics.push_back(message.str());
}

static std::vector<std::string> roomMeasurementIds(yyjson_val *parameters, std::uint32_t width) {
  const auto shared = std::string(jsonText(yyjson_obj_get(parameters, "ms")));
  auto ids = std::vector<std::string>{};
  auto assigned = false;
  // Slots are relative to the selected processing window. Single-channel
  // selections have no per-channel rows in the application and use shared ms.
  if (width > 1) {
    auto *array = yyjson_obj_get(parameters, "ms");
    for (auto channel = 0u; channel < width; ++channel) {
      const auto key = "ms" + std::to_string(channel);
      auto *value = yyjson_obj_get(parameters, key.c_str());
      if (!value || yyjson_is_null(value)) value = yyjson_arr_get(array, channel);
      auto id = std::string(jsonText(value));
      assigned = assigned || !id.empty();
      ids.push_back(id.empty() ? shared : std::move(id));
    }
  }
  return assigned ? ids : std::vector<std::string>{shared};
}

static std::string roomCacheIdentity(const std::filesystem::path &directory,
    const assets::RoomEqConfiguration &config, std::span<const std::string> ids,
    std::span<const AssetFileSnapshot> snapshots, std::uint32_t width, std::uint32_t head) {
  auto identity = std::ostringstream{};
  identity.imbue(std::locale::classic());
  identity << "effetune:6791afdfc6f4e6c913b9f48c8189a44e29a09bde/room-v1/ETA1" << '\0'
           << directory.lexically_normal().string() << '\0' << std::setprecision(17)
           << config.sampleRate << '/' << config.taps << '/' << static_cast<unsigned>(config.phase) << '/'
           << width << '/' << head << '/' << config.smoothing << '/' << config.lowFrequency << '/'
           << config.highFrequency << '/' << config.maxBoostDb << '/' << config.correctionAmount << '/'
           << config.directWindowMs << '/' << config.phaseLowFrequency.value_or(-1) << '/'
           << config.phaseCorrectionAmount << '/' << config.lowFrequencyPhaseExtension << '/'
           << config.reverbAmount << '/' << config.reverbWindowMs << '/' << config.reverbMaxFrequency << '/'
           << config.reverbSmoothing << '/' << config.phaseSmoothing.value_or(-1) << '/' << config.referencePoint << '\0';
  for (const auto &band : config.eqBands)
    identity << static_cast<unsigned>(band.type) << '/' << band.enabled << '/' << band.frequency << '/'
             << band.gain << '/' << band.q << '\0';
  identity << "sources" << '\0';
  for (const auto &id : ids) identity << id << '\0';
  for (const auto &snapshot : snapshots)
    identity << snapshot.path.lexically_normal().string() << '\0' << snapshot.bytes << '/' << snapshot.digest << '\0';
  const auto bytes = identity.str();
  return assetSha256({reinterpret_cast<const std::uint8_t *>(bytes.data()), bytes.size()});
}

static cardio::promise<std::shared_ptr<const SharedPreparedAsset>> prepareRoomAsset(const std::filesystem::path &directory,
    yyjson_val *parameters, float sampleRate, std::uint32_t processingChannels,
    const AssetCacheOptions &cache, cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes,
    RoomAssetLoadResult &result, SharedPreparedAsset &owned, AssetMemoryReservation &memory, std::string &sharingKey) {
  if (!processingChannels || processingChannels > 16) throw std::invalid_argument("selected Room EQ channels are unavailable");
  const auto ids = roomMeasurementIds(parameters, processingChannels);
  auto *delay = yyjson_obj_get(parameters, "dl");
  if (!delay || yyjson_is_null(delay)) delay = yyjson_obj_get(parameters, "dy0");
  if (delay && !yyjson_is_null(delay)) {
    const auto key = yyjson_obj_get(parameters, "dl") == delay ? "dl" : "dy0";
    result.channelDelaySamples = static_cast<std::uint32_t>(std::min(3840.0,
        std::round(roomNumber(parameters, key, 0, 0, 20) * sampleRate / 1000)));
  } else result.channelDelaySamples = static_cast<std::uint32_t>(std::round(roomNumber(parameters, "dy", 0, 0, 3840)));
  if (std::ranges::all_of(ids, [](const auto &id) { return id.empty(); })) {
    owned.diagnostics.emplace_back("No measurement is assigned; Room EQ gain and delay remain active");
    co_return nullptr;
  }
  auto config = roomConfiguration(parameters, sampleRate);
  if (processingChannels > 8) config.taps = std::min(config.taps, 65536u);
  const auto head = roomNumber(parameters, "lt", 128, 0, 1024);
  if (head != 0 && head != 128 && head != 256 && head != 512 && head != 1024)
    throw std::invalid_argument("invalid Room EQ convolution latency");
  // A shared measurement produces one mono FIR, as in the application. The
  // convolver applies it to every selected channel without duplicating IR data.
  auto sources = std::vector<assets::RoomEqSource>(ids.size());
  auto references = std::vector<MeasurementReference>(ids.size());
  if (directory.empty()) throw std::runtime_error("EffeTune measurement directory is unavailable");
  for (auto channel = std::size_t{0}; channel < ids.size(); ++channel) {
    const auto &id = ids[channel];
    if (id.empty()) continue;
    const auto reference = parseMeasurementReference(id);
    references[channel] = reference;
    const auto path = directory / (reference.id + ".json");
    if (std::ranges::find(result.files, path) == result.files.end()) result.files.push_back(path);
  }
  auto sourceBytes = std::uint64_t{0};
  memory.resize(128 * 1024, maximumMemoryBytes);
  for (const auto &path : result.files) {
    auto inspected = std::move(co_await inspectAssetFile(path, 64u * 1024u * 1024u, 0, cancellation));
    sourceBytes += inspected.snapshot.bytes;
    result.snapshots.push_back(std::move(inspected.snapshot));
  }
  const auto key = roomCacheIdentity(directory, config, ids, result.snapshots, processingChannels, static_cast<std::uint32_t>(head));
  sharingKey = cache.directory.lexically_normal().string() + '\0' + std::to_string(cache.maximumBytes) + '\0' + key;
  auto &entries = assetPreparationPool().entries;
  std::erase_if(entries, [](const auto &entry) { return entry.second.expired(); });
  if (const auto found = entries.find(sharingKey); found != entries.end()) {
    if (auto reused = found->second.lock()) {
      co_await verifyAssetSnapshots(result.snapshots, cancellation);
      result.cacheHit = true;
      co_return reused;
    }
  }
  // Cache metadata and payload are validated before decoding any measurement.
  // Its largest allocation is bounded independently of the source JSON workspace.
  memory.resize(40ull * 1024 * 1024, maximumMemoryBytes);
  auto cached = std::move(co_await readAssetCache(cache, key, 32u * 1024u * 1024u, cancellation));
  if (cached) {
    co_await verifyAssetSnapshots(result.snapshots, cancellation);
    result.cacheHit = true;
    owned.asset = std::move(cached->asset); owned.diagnostics = std::move(cached->diagnostics);
    owned.cachePin = std::move(cached->pin);
    co_return nullptr;
  }
  {
    // JSON nodes, copied response pairs, FIRs and FFT temporaries are all admitted
    // before allocating. The source limit also rejects growth after inspection.
    memory.resize(128ull * 1024 * 1024 + sourceBytes * (32 + 8 * processingChannels), maximumMemoryBytes);
    auto store = MeasurementStore{directory, {}};
    for (auto channel = std::size_t{0}; channel < ids.size(); ++channel) {
      if (ids[channel].empty()) continue;
      const auto &reference = references[channel];
      const auto snapshot = std::ranges::find_if(result.snapshots, [&](const auto &value) {
        return value.path == directory / (reference.id + ".json");
      });
      auto *document = co_await readMeasurementSessionAsync(store, reference.id, snapshot->bytes, cancellation);
      if (store.snapshots.at(reference.id).digest != snapshot->digest)
        throw std::runtime_error("Room EQ measurement changed during preparation");
      const auto response = measurementFrequencyResponse(yyjson_doc_get_root(document), reference.channel);
      auto measured = measurementImpulseResponses(yyjson_doc_get_root(document), reference.channel);
      auto &source = sources[channel];
      source.assigned = true;
      for (const auto &[frequency, magnitude] : response) source.response.push_back({frequency, magnitude});
      for (auto &point : measured) {
        source.impulses.push_back({std::move(point.impulse.samples), static_cast<std::uint32_t>(point.impulse.sampleRate),
            static_cast<std::uint64_t>(point.impulse.onsetIndex), point.impulse.referenceScale, point.pointId});
      }
    }
  }
  auto resampledFrames = std::uint64_t{0}, maximumFft = std::uint64_t{0}, pointCount = std::uint64_t{0};
  for (const auto &source : sources) for (const auto &impulse : source.impulses) {
    const auto frames = static_cast<std::uint64_t>(std::max(1.0,
        std::floor(static_cast<double>(impulse.samples.size()) * config.sampleRate / impulse.sampleRate + 0.5)));
    resampledFrames += frames;
    maximumFft = std::max(maximumFft, std::bit_ceil(frames));
    ++pointCount;
  }
  // The JSON temporaries are gone. Admit rate expansion and the largest FFT
  // workspace before the pure design starts; a long low-rate IR cannot evade
  // the process budget merely because its saved representation is small.
  memory.resize(128ull * 1024 * 1024 + resampledFrames * 24 + maximumFft * 768 +
      static_cast<std::uint64_t>(config.taps) * 256 +
      pointCount * (static_cast<std::uint64_t>(config.taps) + 1) * 128, maximumMemoryBytes);
  co_await preparationCheckpoint(cancellation);
  auto designed = assets::designRoomEq(config, sources);
  auto convolution = assets::ConvolutionConfig{};
  convolution.topology = sources.size() == 1 ? assets::Topology::mono : assets::Topology::independent;
  convolution.processingChannels = processingChannels;
  convolution.headBlock = static_cast<std::uint32_t>(head);
  auto encoded = assets::encodeConvolution({config.sampleRate, std::move(designed.channels)}, convolution);
  owned.asset.filterDelaySamples = designed.filterDelaySamples;
  owned.asset.info = {.channels = encoded.channels, .frames = encoded.frames,
      .topology = static_cast<std::uint32_t>(convolution.topology), .head_block = convolution.headBlock,
      .rate_divider = 1, .path_count = 0, .input_count = 0, .processing_channels = processingChannels,
      .footprint_bytes = static_cast<std::uint32_t>(encoded.footprintBytes), .byte_size = static_cast<std::uint32_t>(encoded.payload.size())};
  owned.asset.payload = std::move(encoded.payload);
  for (const auto &warning : designed.qualityWarnings) owned.diagnostics.push_back("Room EQ: " + warning);
  for (const auto &diagnostic : designed.diagnostics.phaseCorrection)
    appendRoomDiagnostic(owned, "phase correction", diagnostic, false);
  for (const auto &diagnostic : designed.diagnostics.lowFrequencyPhaseExtension)
    appendRoomDiagnostic(owned, "low-frequency extension", diagnostic, false);
  for (const auto &diagnostic : designed.diagnostics.reverbCorrection)
    appendRoomDiagnostic(owned, "reverb correction", diagnostic, true);
  co_await preparationCheckpoint(cancellation);
  co_await verifyAssetSnapshots(result.snapshots, cancellation);
  auto saved = std::move(co_await writeAssetCache(cache, key, owned.asset, owned.diagnostics, cancellation));
  owned.cachePin = std::move(saved.pin);
  if (!saved.error.empty()) owned.diagnostics.push_back("Prepared Room EQ filter is usable, but its cache could not be saved: " + saved.error);
  co_await verifyAssetSnapshots(result.snapshots, cancellation);
  co_return nullptr;
}

cardio::promise<RoomAssetLoadResult> loadRoomAssetAsync(const std::filesystem::path &directory,
    yyjson_val *parameters, float sampleRate, std::uint32_t processingChannels,
    const AssetCacheOptions &cache, cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes) {
  auto result = RoomAssetLoadResult{};
  try {
    auto &pool = assetPreparationPool();
    const auto preparation = std::move(co_await pool.mutex.lock(cancellation));
    auto memory = AssetMemoryReservation{};
    auto owned = std::make_shared<SharedPreparedAsset>();
    auto sharingKey = std::string{};
    result.prepared = std::move(co_await prepareRoomAsset(directory, parameters, sampleRate, processingChannels,
        cache, cancellation, maximumMemoryBytes, result, *owned, memory, sharingKey));
    if (!result.prepared) {
      memory.shrink(owned->asset.payload.capacity());
      owned->memory = std::move(memory);
      result.prepared = std::move(owned);
      if (!sharingKey.empty()) pool.entries[sharingKey] = result.prepared;
    }
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const std::exception &error) { result.error = std::string("Room EQ: ") + error.what(); }
  co_return result;
}

} // namespace pipetune
