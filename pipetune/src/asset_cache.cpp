/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_cache.h"
#include "asset_file.h"

#include <yyjson.h>
#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>

namespace pipetune {

template <typename T>
using CacheObject = std::unique_ptr<T, decltype(&g_object_unref)>;

static constexpr auto kHeaderBytes = std::size_t{80};
static constexpr auto kMetadataLimit = std::size_t{65536};
static constexpr auto kPayloadLimit = std::size_t{1024u * 1024u * 1024u};
static constexpr auto kMagic = std::string_view{"PTAC0001"};

static std::mutex pinMutex;
static std::unordered_map<std::string, std::weak_ptr<const AssetCachePin>> pins;
// Reclamation and atomic replacement form one transaction across dispatchers.
// Each waiter belongs to its own dispatcher; no promise is shared across groups.
static cardio::primitives::mutex publicationMutex;

static bool validKey(std::string_view key) {
  return key.size() == 64 && std::ranges::all_of(key,
      [](char value) { return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

static std::filesystem::path cachePath(const AssetCacheOptions &options, std::string_view key) {
  return options.directory / "assets-v1" / (std::string(key) + ".ptac");
}

static std::shared_ptr<const AssetCachePin> pinEntry(const std::filesystem::path &path) {
  auto lock = std::scoped_lock(pinMutex);
  std::erase_if(pins, [](const auto &entry) { return entry.second.expired(); });
  auto &weak = pins[path.lexically_normal().string()];
  auto pin = weak.lock();
  if (!pin) {
    pin = std::make_shared<const AssetCachePin>(AssetCachePin{path});
    weak = pin;
  }
  return pin;
}

static bool isPinned(const std::filesystem::path &path) {
  auto lock = std::scoped_lock(pinMutex);
  const auto entry = pins.find(path.lexically_normal().string());
  return entry != pins.end() && !entry->second.expired();
}

static std::string metadata(const PreparedDspAsset &asset, std::string_view key,
                             const std::vector<std::string> &diagnostics) {
  const auto document = std::unique_ptr<yyjson_mut_doc, decltype(&yyjson_mut_doc_free)>(
      yyjson_mut_doc_new(nullptr), yyjson_mut_doc_free);
  if (!document) throw std::bad_alloc();
  auto *root = yyjson_mut_obj(document.get());
  yyjson_mut_doc_set_root(document.get(), root);
  if (!yyjson_mut_obj_add_strncpy(document.get(), root, "key", key.data(), key.size())) throw std::bad_alloc();
  auto *info = yyjson_mut_obj_add_arr(document.get(), root, "info");
  const auto &value = asset.info;
  const auto fields = std::array{value.channels, value.frames, value.topology, value.head_block,
      value.rate_divider, value.path_count, value.input_count, value.processing_channels,
      value.footprint_bytes, value.byte_size, asset.formatTag, asset.bandCount, asset.filterDelaySamples, asset.warmupFrames};
  for (const auto field : fields)
    if (!yyjson_mut_arr_add_uint(document.get(), info, field)) throw std::bad_alloc();
  auto *messages = yyjson_mut_obj_add_arr(document.get(), root, "diagnostics");
  if (!messages) throw std::bad_alloc();
  for (const auto &message : diagnostics) {
    if (message.empty()) throw std::invalid_argument("empty cache diagnostic");
    if (!yyjson_mut_arr_add_strncpy(document.get(), messages, message.data(), message.size())) throw std::bad_alloc();
  }
  auto length = std::size_t{0};
  const auto bytes = std::unique_ptr<char, decltype(&std::free)>(yyjson_mut_write(document.get(), 0, &length), std::free);
  if (!bytes) throw std::bad_alloc();
  if (length > kMetadataLimit) throw std::length_error("asset cache metadata exceeds its byte limit");
  return {bytes.get(), length};
}

static std::string contentDigest(std::string_view metadata, std::span<const std::uint8_t> payload) {
  const auto digest = std::unique_ptr<GChecksum, decltype(&g_checksum_free)>(
      g_checksum_new(G_CHECKSUM_SHA256), g_checksum_free);
  if (!digest) throw std::bad_alloc();
  g_checksum_update(digest.get(), reinterpret_cast<const guchar *>(metadata.data()), metadata.size());
  g_checksum_update(digest.get(), payload.data(), payload.size());
  return g_checksum_get_string(digest.get());
}

static std::array<std::uint8_t, kHeaderBytes> makeHeader(
    std::string_view metadata, std::span<const std::uint8_t> payload) {
  auto header = std::array<std::uint8_t, kHeaderBytes>{};
  std::copy(kMagic.begin(), kMagic.end(), header.begin());
  const auto put = [&header](std::size_t offset, std::uint32_t value) {
    for (auto byte = 0u; byte < 4u; ++byte) header[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
  };
  put(8, metadata.size());
  put(12, payload.size());
  const auto digest = contentDigest(metadata, payload);
  std::copy(digest.begin(), digest.end(), header.begin() + 16);
  return header;
}

static CachedDspAsset decodeEntry(std::vector<std::uint8_t> bytes, std::string_view key,
                                  std::size_t maximumPayloadBytes) {
  if (bytes.size() < kHeaderBytes || !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()))
    throw std::runtime_error("obsolete asset cache format");
  const auto get = [&bytes](std::size_t offset) {
    auto value = std::uint32_t{0};
    for (auto byte = 0u; byte < 4u; ++byte) value |= static_cast<std::uint32_t>(bytes[offset + byte]) << (byte * 8);
    return value;
  };
  const auto metadataBytes = get(8);
  const auto payloadBytes = get(12);
  if (metadataBytes > kMetadataLimit || payloadBytes == 0 || payloadBytes > maximumPayloadBytes ||
      kHeaderBytes + static_cast<std::uint64_t>(metadataBytes) + payloadBytes != bytes.size())
    throw std::runtime_error("invalid asset cache lengths");
  const auto text = std::string_view(reinterpret_cast<const char *>(bytes.data() + kHeaderBytes), metadataBytes);
  const auto digest = std::string_view(reinterpret_cast<const char *>(bytes.data() + 16), 64);
  if (assetSha256(std::span(bytes).subspan(kHeaderBytes)) != digest)
    throw std::runtime_error("asset cache checksum mismatch");
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(text.data(), text.size(), 0), yyjson_doc_free);
  auto *root = document ? yyjson_doc_get_root(document.get()) : nullptr;
  auto *storedKey = yyjson_obj_get(root, "key");
  auto *info = yyjson_obj_get(root, "info");
  auto *messages = yyjson_obj_get(root, "diagnostics");
  if (!yyjson_is_str(storedKey) || std::string_view(yyjson_get_str(storedKey), yyjson_get_len(storedKey)) != key ||
      !yyjson_is_arr(info) || yyjson_arr_size(info) != 14 || !yyjson_is_arr(messages))
    throw std::runtime_error("invalid asset cache metadata");
  auto result = CachedDspAsset{};
  auto &value = result.asset.info;
  const auto fields = std::array{&value.channels, &value.frames, &value.topology, &value.head_block,
      &value.rate_divider, &value.path_count, &value.input_count, &value.processing_channels,
      &value.footprint_bytes, &value.byte_size, &result.asset.formatTag, &result.asset.bandCount, &result.asset.filterDelaySamples,
      &result.asset.warmupFrames};
  for (auto index = std::size_t{0}; index < fields.size(); ++index) {
    auto *field = yyjson_arr_get(info, index);
    if (!yyjson_is_uint(field) || yyjson_get_uint(field) > std::numeric_limits<std::uint32_t>::max())
      throw std::runtime_error("invalid cached native asset field");
    *fields[index] = static_cast<std::uint32_t>(yyjson_get_uint(field));
  }
  if (value.byte_size != payloadBytes) throw std::runtime_error("cached copy-ABI length differs");
  for (auto index = std::size_t{0}; index < yyjson_arr_size(messages); ++index) {
    auto *message = yyjson_arr_get(messages, index);
    if (!yyjson_is_str(message) || yyjson_get_len(message) == 0) throw std::runtime_error("invalid cache diagnostic");
    result.diagnostics.emplace_back(yyjson_get_str(message), yyjson_get_len(message));
  }
  // Reuse the bounded read allocation instead of allocating another bank-sized buffer.
  bytes.erase(bytes.begin(), bytes.begin() + kHeaderBytes + metadataBytes);
  result.asset.payload = std::move(bytes);
  return result;
}

static cardio::promise<void> touchEntry(GFile *file) {
  const auto info = CacheObject<GFileInfo>(g_file_info_new(), g_object_unref);
  const auto now = static_cast<std::uint64_t>(g_get_real_time());
  g_file_info_set_attribute_uint64(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED, now / 1000000);
  g_file_info_set_attribute_uint32(info.get(), G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC, now % 1000000);
  const auto updated = CacheObject<GFileInfo>(co_await cardio::gio::submit<GFileInfo *>(
      [file, info = info.get()](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
        g_file_set_attributes_async(file, info, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                    G_PRIORITY_DEFAULT, cancel, callback, data);
      },
      [](GObject *object, GAsyncResult *result, GError **error) {
        auto *updated = static_cast<GFileInfo *>(nullptr);
        const auto success = g_file_set_attributes_finish(G_FILE(object), result, &updated, error);
        if (!success && updated) { g_object_unref(updated); updated = nullptr; }
        return updated;
      }), g_object_unref);
  if (!updated) throw std::runtime_error("cannot update asset cache age");
}

static cardio::promise<void> ensureDirectory(const std::filesystem::path &path) {
  if (path.empty() || path == path.root_path()) co_return;
  const auto file = CacheObject<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  auto existing = CacheObject<GFileInfo>(nullptr, g_object_unref);
  try { existing.reset(co_await cardio::gio::query_info(file.get(), "standard::type", G_FILE_QUERY_INFO_NONE)); }
  catch (const std::exception &) {}
  if (existing) {
    if (g_file_info_get_file_type(existing.get()) != G_FILE_TYPE_DIRECTORY)
      throw std::runtime_error("asset cache path is not a directory");
    co_return;
  }
  co_await ensureDirectory(path.parent_path());
  auto failure = std::exception_ptr{};
  try { co_await cardio::gio::make_directory(file.get()); }
  catch (...) { failure = std::current_exception(); }
  if (failure) {
    const auto current = CacheObject<GFileInfo>(
        co_await cardio::gio::query_info(file.get(), "standard::type", G_FILE_QUERY_INFO_NONE), g_object_unref);
    if (g_file_info_get_file_type(current.get()) != G_FILE_TYPE_DIRECTORY) std::rethrow_exception(failure);
  }
}

struct DiskEntry {
  std::filesystem::path path;
  std::uint64_t bytes;
  std::uint64_t modified;
};

static cardio::promise<void> reclaim(const AssetCacheOptions &options, std::uint64_t required) {
  if (required > options.maximumBytes) throw std::length_error("asset exceeds the cache disk budget");
  const auto directory = options.directory / "assets-v1";
  const auto file = CacheObject<GFile>(g_file_new_for_path(directory.c_str()), g_object_unref);
  const auto iterator = CacheObject<GFileEnumerator>(co_await cardio::gio::enumerate_children(file.get(),
      "standard::name,standard::type,standard::size,time::modified,time::modified-usec",
      G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS), g_object_unref);
  auto entries = std::vector<DiskEntry>{};
  auto total = std::uint64_t{0};
  auto failure = std::exception_ptr{};
  try {
    for (;;) {
      const auto freeList = [](GList *list) { g_list_free_full(list, g_object_unref); };
      const auto batch = std::unique_ptr<GList, decltype(freeList)>(
          co_await cardio::gio::next_files(iterator.get(), 64), freeList);
      if (!batch) break;
      for (auto *item = batch.get(); item != nullptr; item = item->next) {
        auto *info = G_FILE_INFO(item->data);
        const auto *name = g_file_info_get_name(info);
        const auto size = g_file_info_get_size(info);
        if (!name || size < 0 || g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR) continue;
        const auto filename = std::string_view(name);
        if (!filename.ends_with(".ptac") || !validKey(filename.substr(0, filename.size() - 5))) continue;
        const auto bytes = static_cast<std::uint64_t>(size);
        if (bytes > std::numeric_limits<std::uint64_t>::max() - total) throw std::length_error("cache disk usage overflow");
        total += bytes;
        const auto seconds = g_file_info_get_attribute_uint64(info, G_FILE_ATTRIBUTE_TIME_MODIFIED);
        const auto micros = g_file_info_get_attribute_uint32(info, G_FILE_ATTRIBUTE_TIME_MODIFIED_USEC);
        const auto modified = seconds > (std::numeric_limits<std::uint64_t>::max() - micros) / 1000000 ?
            std::numeric_limits<std::uint64_t>::max() : seconds * 1000000 + micros;
        entries.push_back({directory / filename, bytes, modified});
      }
    }
  } catch (...) { failure = std::current_exception(); }
  co_await cardio::gio::close(iterator.get());
  if (failure) std::rethrow_exception(failure);
  std::ranges::sort(entries, {}, &DiskEntry::modified);
  for (const auto &entry : entries) {
    if (total <= options.maximumBytes - required) break;
    if (isPinned(entry.path)) continue;
    const auto old = CacheObject<GFile>(g_file_new_for_path(entry.path.c_str()), g_object_unref);
    co_await cardio::gio::delete_file(old.get());
    total -= entry.bytes;
  }
  if (total > options.maximumBytes - required) throw std::length_error("cache disk budget is occupied by active assets");
}

static cardio::promise<void> writeBytes(GOutputStream *stream, std::span<const std::byte> bytes,
                                        cardio::cancellation cancellation) {
  while (!bytes.empty()) {
    cancellation.throw_if_cancellation_requested();
    const auto count = co_await cardio::gio::write(stream, bytes.first(std::min<std::size_t>(bytes.size(), 1024u * 1024u)));
    if (count == 0) throw std::runtime_error("cache stream stopped accepting bytes");
    bytes = bytes.subspan(count);
  }
}

cardio::promise<std::optional<CachedDspAsset>> readAssetCache(
    const AssetCacheOptions &options, std::string_view key, std::size_t maximumPayloadBytes,
    cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (options.directory.empty() || !validKey(key) || options.maximumBytes == 0) co_return std::nullopt;
  const auto publication = std::move(co_await publicationMutex.lock(cancellation));
  const auto path = cachePath(options, key);
  const auto pin = pinEntry(path);
  try {
    const auto limit = std::min(maximumPayloadBytes, kPayloadLimit);
    // cardio's await result is a reference; consume this single-use result
    // explicitly so a large bank is not copied at the coroutine boundary.
    auto bytes = std::move(co_await readAssetFile(path, std::min<std::uint64_t>(
        limit + kHeaderBytes + kMetadataLimit, options.maximumBytes), cancellation));
    auto result = decodeEntry(std::move(bytes), key, limit);
    result.pin = pin;
    const auto file = CacheObject<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
    // A read-only but otherwise valid cache remains useful.
    try { co_await touchEntry(file.get()); } catch (const std::exception &) {}
    cancellation.throw_if_cancellation_requested();
    co_return result;
  } catch (const cardio::canceled_exception &) { throw;
  } catch (const std::bad_alloc &) { throw; }
  catch (const std::exception &) { co_return std::nullopt; }
}

cardio::promise<AssetCacheWriteResult> writeAssetCache(
    const AssetCacheOptions &options, std::string_view key, const PreparedDspAsset &asset,
    const std::vector<std::string> &diagnostics, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  if (options.directory.empty()) co_return AssetCacheWriteResult{};
  try {
    const auto publication = std::move(co_await publicationMutex.lock(cancellation));
    if (!validKey(key) || asset.payload.empty() || asset.payload.size() > kPayloadLimit ||
        asset.info.byte_size != asset.payload.size() || !asset.error.empty() || !asset.omissionReason.empty())
      throw std::invalid_argument("invalid prepared cache entry");
    const auto path = cachePath(options, key);
    const auto pin = pinEntry(path);
    const auto text = metadata(asset, key, diagnostics);
    const auto header = makeHeader(text, asset.payload);
    co_await ensureDirectory(path.parent_path());
    // Include the replacement's temporary file in the budget while the old file
    // still exists. Pins retain only identity; the native engine owns its copy.
    co_await reclaim(options, header.size() + text.size() + asset.payload.size());
    const auto file = CacheObject<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
    const auto stream = CacheObject<GFileOutputStream>(co_await cardio::gio::replace(file.get(), nullptr, false,
        static_cast<GFileCreateFlags>(G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION)), g_object_unref);
    auto failure = std::exception_ptr{};
    try {
      co_await writeBytes(G_OUTPUT_STREAM(stream.get()), std::as_bytes(std::span(header)), cancellation);
      co_await writeBytes(G_OUTPUT_STREAM(stream.get()), std::as_bytes(std::span(text)), cancellation);
      co_await writeBytes(G_OUTPUT_STREAM(stream.get()), std::as_bytes(std::span(asset.payload)), cancellation);
    } catch (...) { failure = std::current_exception(); }
    co_await cardio::gio::close(G_OUTPUT_STREAM(stream.get()));
    if (failure) std::rethrow_exception(failure);
    cancellation.throw_if_cancellation_requested();
    co_return AssetCacheWriteResult{pin, {}};
  } catch (const cardio::canceled_exception &) { throw;
  } catch (const std::exception &error) {
    co_return AssetCacheWriteResult{{}, error.what()};
  }
}

} // namespace pipetune
