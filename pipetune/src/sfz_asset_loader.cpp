/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "sfz_asset_loader.h"
#include "asset_audio_decoder.h"
#include "asset_cache.h"
#include "preparation_dispatcher.h"
#include "sfz/audio_header.h"
#include "sfz/preparation.h"
#include "sfz/selection.h"
#include "sfz/text.h"
#include <algorithm>
#include <array>
#include <map>
#include <sstream>

namespace pipetune {

static std::string jsonString(yyjson_val *value) {
  return yyjson_is_str(value) ? std::string(yyjson_get_str(value), yyjson_get_len(value)) : std::string{};
}

static bool validId(std::string_view value) {
  return value.size() == 24 && std::ranges::all_of(value,
      [](char ch) { return (ch >= 'a' && ch <= 'f') || (ch >= '0' && ch <= '9'); });
}

struct SfzReference {
  std::filesystem::path root;
  std::string selected;
};

static SfzReference referenceFromRegistry(std::span<const std::uint8_t> bytes, std::string_view id) {
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(reinterpret_cast<const char *>(bytes.data()), bytes.size(), 0), yyjson_doc_free);
  auto *entries = document ? yyjson_doc_get_root(document.get()) : nullptr;
  if (!yyjson_is_arr(entries) || yyjson_arr_size(entries) > 10000)
    throw std::runtime_error("SFZ registry is not a valid file list");
  auto selected = SfzReference{};
  for (auto index = std::size_t{0}; index < yyjson_arr_size(entries); ++index) {
    auto *entry = yyjson_arr_get(entries, index);
    const auto entryId = jsonString(yyjson_obj_get(entry, "id"));
    const auto source = jsonString(yyjson_obj_get(entry, "path"));
    const auto folder = jsonString(yyjson_obj_get(entry, "root"));
    const auto path = std::filesystem::path(source), root = std::filesystem::path(folder);
    const auto relative = path.lexically_relative(root);
    auto extension = path.extension().string();
    for (auto &ch : extension) if (ch >= 'A' && ch <= 'Z') ch += 'a' - 'A';
    if (!validId(entryId) || !yyjson_is_str(yyjson_obj_get(entry, "name")) ||
        source.find('\0') != std::string::npos || folder.find('\0') != std::string::npos ||
        !path.is_absolute() || !root.is_absolute() || relative.empty() || relative == "." ||
        relative.is_absolute() || *relative.begin() == ".." || extension != ".sfz")
      throw std::runtime_error("SFZ registry contains an invalid reference");
    if (entryId == id) selected = {root, assets::normalizeSfzPath(relative.generic_string(), {})};
  }
  if (selected.root.empty()) throw std::runtime_error("the assigned SFZ ID is not registered in EffeTune");
  return selected;
}

static std::uint64_t parsedMemory(const assets::ParsedSfz &parsed) {
  auto bytes = static_cast<std::uint64_t>(parsed.regions.capacity()) * sizeof(assets::SfzRegion);
  for (const auto &region : parsed.regions) bytes += region.sample.capacity() + 1;
  for (const auto *strings : {&parsed.dependencies, &parsed.diagnostics.ignoredOpcodes,
      &parsed.diagnostics.excludedOpcodes, &parsed.diagnostics.invalidRegions, &parsed.diagnostics.missingSamples}) {
    bytes += strings->capacity() * sizeof(std::string);
    for (const auto &text : *strings) bytes += text.capacity() + 1;
  }
  return bytes;
}

static std::uint64_t sourceOverhead(const SfzAssetLoadResult &result) {
  auto bytes = 64ull * 1024 * 1024;
  for (const auto &file : result.files) bytes += 8 * (sizeof(file) + file.native().capacity() + 1);
  for (const auto &snapshot : result.snapshots) bytes += 8 * (sizeof(snapshot) + snapshot.path.native().capacity() +
      snapshot.root.native().capacity() + snapshot.resolvedPath.native().capacity() + snapshot.digest.capacity() + 4);
  return bytes;
}

static void describeWarnings(const assets::ParsedSfz &parsed, const assets::SfzBank &bank,
    std::vector<std::string> &diagnostics) {
  for (const auto &warning : bank.warnings) {
    const auto explanation = warning.code == "invalid-regions" ? "invalid regions were omitted" :
        warning.code == "missing-samples" ? "missing sample files were omitted" :
        warning.code == "unsupported-regions" ? "regions requiring unsupported conditions were omitted" :
        warning.code == "reduced-bank" ? "layers were reduced while preserving the complete key range" :
        "unused invalid loop points were repaired";
    diagnostics.push_back("SFZ " + warning.code + ": " + std::to_string(warning.count) + "; " + explanation);
  }
  if (!parsed.diagnostics.ignoredOpcodes.empty()) {
    auto message = std::string("SFZ ignored opcodes: ");
    for (auto index = std::size_t{0}; index < std::min<std::size_t>(8, parsed.diagnostics.ignoredOpcodes.size()); ++index) {
      if (index) message += ", ";
      message += parsed.diagnostics.ignoredOpcodes[index];
    }
    if (parsed.diagnostics.ignoredOpcodes.size() > 8) message += ", ...";
    diagnostics.push_back(std::move(message));
  }
}

static std::string cacheIdentity(const SfzReference &reference, std::string_view id, std::uint64_t maximumBytes,
    std::span<const AssetFileSnapshot> snapshots) {
  auto identity = std::ostringstream{};
  identity << "effetune:6791afdfc6f4e6c913b9f48c8189a44e29a09bde/sfz-v1/ETA1-table-v2" << '\0'
      << assetDecoderVersion() << '\0' << reference.root.generic_string() << '\0' << reference.selected << '\0'
      << id << '\0' << maximumBytes << '\0';
  for (const auto &source : snapshots) identity << source.path.generic_string() << '\0'
      << source.resolvedPath.generic_string() << '\0' << source.bytes << '/' << source.missing << '\0' << source.digest << '\0';
  const auto text = identity.str();
  return assetSha256({reinterpret_cast<const std::uint8_t *>(text.data()), text.size()});
}

static cardio::promise<std::shared_ptr<const SharedPreparedAsset>> prepareRegisteredSfz(
    const std::filesystem::path &directory, std::string id, std::uint64_t maximumBytes,
    const AssetCacheOptions &cache, cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes,
    AssetMemoryReservation &memory, SfzAssetLoadResult &loadResult, SharedPreparedAsset &owned, std::string &sharingKey) {
  co_await preparationCheckpoint(cancellation);
  if (id.empty()) {
    owned.diagnostics.emplace_back("No SFZ instrument is assigned; the dry settings remain active");
    co_return nullptr;
  }
  if (!validId(id)) throw std::runtime_error("SFZ ID must contain 24 lowercase hexadecimal digits");
  if (directory.empty()) throw std::runtime_error("EffeTune data directory is unavailable");
  const auto registryPath = directory / "sfz-references.json";
  loadResult.files.push_back(registryPath);
  auto registry = std::move(co_await inspectAssetFile(registryPath, 1024 * 1024, 1024 * 1024, cancellation));
  const auto reference = referenceFromRegistry(registry.prefix, id);
  std::vector<std::uint8_t>().swap(registry.prefix);
  auto parserWorkingBytes = std::uint64_t{16 * 1024 * 1024};
  auto documents = std::vector<assets::SfzDocument>{};
  auto definitionBytes = std::uint64_t{0};
  auto parsed = assets::ParsedSfz{};
  const auto parseCurrent = [&](std::optional<std::span<const std::string>> available) {
    for (;;) {
      cancellation.throw_if_cancellation_requested();
      const auto overhead = sourceOverhead(loadResult);
      if (parserWorkingBytes > maximumMemoryBytes || definitionBytes > maximumMemoryBytes - parserWorkingBytes ||
          overhead > maximumMemoryBytes - parserWorkingBytes - definitionBytes)
        throw std::length_error("combined asset memory budget cannot admit SFZ definition preparation");
      memory.resize(definitionBytes + parserWorkingBytes + overhead, maximumMemoryBytes);
      try { return assets::parseSfz(reference.selected, documents, available, maximumBytes, parserWorkingBytes); }
      catch (const assets::SfzError &error) {
        if (!error.minimumWorkingBytes) throw;
        // Retry only the pure parser, after reserving its next bounded allowance.
        // A high bank limit must not reserve its entire worst case for a tiny definition.
        parserWorkingBytes = std::max(parserWorkingBytes * 2, error.minimumWorkingBytes);
      }
    }
  };
  for (;;) {
    auto required = std::string{};
    try { parsed = parseCurrent(std::nullopt); }
    catch (const assets::SfzError &error) {
      if (error.requiredPath.empty()) throw;
      required = error.requiredPath;
    }
    if (required.empty()) break;
    if (definitionBytes >= maximumBytes || documents.size() >= 10000)
      throw assets::SfzError("too-large", "The SFZ files are too large to load.");
    const auto path = std::move(co_await resolveConfinedAssetPath(reference.root, required, loadResult.files, cancellation));
    if (!path) throw assets::SfzError("prepare", "An SFZ include could not be found.");
    auto source = std::move(co_await inspectAssetFile(*path, maximumBytes - definitionBytes, 0, cancellation));
    definitionBytes += source.snapshot.bytes;
    memory.resize(definitionBytes + source.snapshot.bytes + parserWorkingBytes + sourceOverhead(loadResult), maximumMemoryBytes);
    const auto bytes = std::move(co_await readAssetFile(*path, source.snapshot.bytes, cancellation));
    if (assetSha256(bytes) != source.snapshot.digest) throw std::runtime_error("SFZ definition changed while loading");
    source.snapshot.path = reference.root / required;
    source.snapshot.root = reference.root;
    source.snapshot.resolvedPath = *path;
    loadResult.snapshots.push_back(std::move(source.snapshot));
    auto text = std::string(bytes.begin(), bytes.end());
    // TextDecoder's default UTF-8 handling removes a leading BOM.
    if (text.starts_with("\xef\xbb\xbf")) text.erase(0, 3);
    documents.push_back({std::move(required), std::move(text)});
    co_await preparationCheckpoint(cancellation);
  }
  memory.resize(definitionBytes + 8 * parsedMemory(parsed) + sourceOverhead(loadResult), maximumMemoryBytes);
  auto paths = std::map<std::u16string, std::string>{};
  for (const auto &region : parsed.regions) paths.try_emplace(assets::sfzUtf16(region.sample), region.sample);
  auto metadata = std::vector<assets::SfzSampleMetadata>{};
  auto available = std::vector<std::string>{};
  for (const auto &[order, sample] : paths) {
    static_cast<void>(order);
    const auto path = std::move(co_await resolveConfinedAssetPath(reference.root, sample, loadResult.files, cancellation));
    memory.resize(definitionBytes + 8 * parsedMemory(parsed) + sourceOverhead(loadResult), maximumMemoryBytes);
    if (!path) {
      loadResult.snapshots.push_back({reference.root / sample, {}, 0, reference.root, {}, true});
      continue;
    }
    auto source = std::move(co_await inspectAssetFile(*path, assets::kMaximumSfzBytes, assets::kSfzAudioHeaderBytes, cancellation));
    const auto header = assets::inspectSfzAudioHeader(source.prefix, source.snapshot.bytes);
    metadata.push_back({sample, source.snapshot.bytes, header.frames, header.channels});
    available.push_back(sample);
    source.snapshot.path = reference.root / sample;
    source.snapshot.root = reference.root;
    source.snapshot.resolvedPath = *path;
    loadResult.snapshots.push_back(std::move(source.snapshot));
  }
  // Drop the first region graph before the second parse; only its sample identities were needed.
  parsed = {};
  co_await preparationCheckpoint(cancellation);
  parsed = parseCurrent(std::span<const std::string>(available));
  if (parsed.regions.empty()) throw assets::SfzError("no-regions", "The SFZ has no playable regions.");
  memory.resize(definitionBytes + 8 * parsedMemory(parsed) + sourceOverhead(loadResult), maximumMemoryBytes);
  auto selection = assets::selectSfzRegionsForBudget(parsed.regions, metadata, maximumBytes, definitionBytes);
  parsed.regions = std::move(selection.regions);
  if (selection.reduced) {
    const auto warning = std::array{assets::SfzWarning{"reduced-bank", 1}};
    parsed.warnings = assets::mergeSfzWarnings(parsed.warnings, warning);
  }
  std::vector<assets::SfzDocument>().swap(documents);
  const auto key = cacheIdentity(reference, id, maximumBytes, loadResult.snapshots);
  loadResult.snapshots.push_back(std::move(registry.snapshot));
  sharingKey = directory.lexically_normal().string() + '\0' + cache.directory.lexically_normal().string() +
      '\0' + std::to_string(cache.maximumBytes) + '\0' + key;
  auto &entries = assetPreparationPool().entries;
  std::erase_if(entries, [](const auto &entry) { return entry.second.expired(); });
  if (const auto found = entries.find(sharingKey); found != entries.end()) {
    if (auto reused = found->second.lock()) {
      co_await verifyAssetSnapshots(loadResult.snapshots, cancellation);
      loadResult.cacheHit = true;
      co_return reused;
    }
  }
  const auto overhead = 8 * parsedMemory(parsed) + sourceOverhead(loadResult);
  memory.resize(overhead, maximumMemoryBytes);
  const auto freeCacheBytes = availableAssetMemoryBytes(maximumMemoryBytes);
  const auto cacheBytes = std::min(maximumBytes, freeCacheBytes > 128 * 1024 ? freeCacheBytes - 128 * 1024 : 0);
  memory.resize(overhead + cacheBytes + (cacheBytes ? 128 * 1024 : 0), maximumMemoryBytes);
  auto cached = std::move(co_await readAssetCache(cache, key, cacheBytes, cancellation));
  if (cached) {
    co_await verifyAssetSnapshots(loadResult.snapshots, cancellation);
    loadResult.cacheHit = true;
    owned.cachePin = std::move(cached->pin);
    owned.diagnostics = std::move(cached->diagnostics);
    owned.asset = std::move(cached->asset);
    co_return nullptr;
  }
  paths.clear();
  for (const auto &region : parsed.regions) paths.try_emplace(assets::sfzUtf16(region.sample), region.sample);
  auto largestOriginal = std::uint64_t{0};
  for (const auto &[order, path] : paths) {
    static_cast<void>(order);
    const auto found = std::ranges::find(metadata, path, &assets::SfzSampleMetadata::path);
    if (found != metadata.end()) largestOriginal = std::max(largestOriginal, found->size);
  }
  // Reserve decoder input/packet storage and PCM growth (old/new vector plus
  // decoder frame), or PCM plus the completed bank. A smaller process limit
  // lowers the incremental decoder allowance instead of rejecting a small
  // instrument merely because its configured bank ceiling is high.
  memory.resize(overhead, maximumMemoryBytes);
  const auto freeDecodeBytes = availableAssetMemoryBytes(maximumMemoryBytes);
  if (largestOriginal > freeDecodeBytes / 2)
    throw std::length_error("combined asset memory budget cannot admit the SFZ original");
  const auto pcmLimit = std::min(maximumBytes, (freeDecodeBytes - 2 * largestOriginal) / 4);
  if (!pcmLimit) throw std::length_error("combined asset memory budget cannot admit SFZ decoding");
  memory.resize(overhead + 2 * largestOriginal + 4 * pcmLimit, maximumMemoryBytes);
  auto samples = std::vector<assets::SfzSample>{};
  auto rawBytes = definitionBytes, decodedBytes = std::uint64_t{0};
  for (const auto &[order, sample] : paths) {
    static_cast<void>(order);
    const auto found = std::ranges::find(metadata, sample, &assets::SfzSampleMetadata::path);
    if (found == metadata.end()) throw assets::SfzError("prepare", "An SFZ sample could not be found.");
    if (found->channels > 2) throw assets::SfzError("prepare", "SFZ samples must contain mono or stereo audio.");
    if (found->frames && found->channels && found->frames > (maximumBytes - decodedBytes) / 4 / found->channels)
      throw assets::SfzError("too-large", "The SFZ samples are too large to load.");
    if (found->frames && found->channels && found->frames > (pcmLimit - decodedBytes) / 4 / found->channels)
      throw std::length_error("combined asset memory budget cannot admit the decoded SFZ samples");
    const auto path = std::move(co_await resolveConfinedAssetPath(reference.root, sample, loadResult.files, cancellation));
    if (!path) throw assets::SfzError("prepare", "An SFZ sample could not be found.");
    const auto bytes = std::move(co_await readAssetFile(*path, maximumBytes - rawBytes, cancellation));
    rawBytes += bytes.size();
    co_await preparationCheckpoint(cancellation);
    auto audio = decodeAssetAudio(bytes, pcmLimit - decodedBytes);
    if (audio.channels.empty() || audio.channels.size() > 2)
      throw assets::SfzError("prepare", "SFZ samples must contain mono or stereo audio.");
    decodedBytes += audio.channels.front().size() * audio.channels.size() * sizeof(float);
    samples.push_back({sample, std::move(audio)});
  }
  co_await preparationCheckpoint(cancellation);
  auto bank = assets::prepareSfzBank(parsed, samples, maximumBytes);
  describeWarnings(parsed, bank, owned.diagnostics);
  owned.asset.info = {.channels = 1, .frames = bank.samples, .topology = 0, .head_block = 0,
      .rate_divider = 1, .path_count = 0, .input_count = 0, .processing_channels = 1,
      .footprint_bytes = static_cast<std::uint32_t>(bank.footprintBytes),
      .byte_size = static_cast<std::uint32_t>(bank.payload.size())};
  owned.asset.warmupFrames = bank.warmupFrames;
  owned.asset.payload = std::move(bank.payload);
  std::vector<assets::SfzSample>().swap(samples);
  co_await verifyAssetSnapshots(loadResult.snapshots, cancellation);
  auto saved = std::move(co_await writeAssetCache(cache, key, owned.asset, owned.diagnostics, cancellation));
  owned.cachePin = std::move(saved.pin);
  if (!saved.error.empty()) owned.diagnostics.push_back("Prepared SFZ instrument is usable, but its cache could not be saved: " + saved.error);
  co_await verifyAssetSnapshots(loadResult.snapshots, cancellation);
  co_return nullptr;
}

cardio::promise<SfzAssetLoadResult> loadSfzAssetAsync(const std::filesystem::path &directory,
    yyjson_val *parameters, std::uint64_t maximumBytes, const AssetCacheOptions &cache,
    cardio::cancellation cancellation, std::uint64_t maximumMemoryBytes) {
  auto result = SfzAssetLoadResult{};
  try {
    if (maximumBytes == 0 || maximumBytes > assets::kMaximumSfzBytes)
      throw assets::SfzError("too-large", "Invalid SFZ size limit.");
    auto &pool = assetPreparationPool();
    const auto preparation = std::move(co_await pool.mutex.lock(cancellation));
    const auto id = jsonString(yyjson_obj_get(parameters, "sf"));
    auto memory = AssetMemoryReservation(id.empty() ? 0 : 64 * 1024 * 1024, maximumMemoryBytes);
    auto owned = std::make_shared<SharedPreparedAsset>();
    auto key = std::string{};
    result.prepared = std::move(co_await prepareRegisteredSfz(directory, id, maximumBytes, cache, cancellation,
        maximumMemoryBytes, memory, result, *owned, key));
    if (!result.prepared) {
      memory.shrink(owned->asset.payload.capacity());
      owned->memory = std::move(memory);
      result.prepared = std::move(owned);
      if (!key.empty()) pool.entries[key] = result.prepared;
    }
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const assets::SfzError &error) { result.error = "SFZ Note Player: " + error.code + ": " + error.what(); }
  catch (const std::exception &error) { result.error = std::string("SFZ Note Player: ") + error.what(); }
  std::ranges::sort(result.files);
  result.files.erase(std::unique(result.files.begin(), result.files.end()), result.files.end());
  co_return result;
}

} // namespace pipetune
