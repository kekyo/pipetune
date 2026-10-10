/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_file.h"

#include <array>
#include <algorithm>
#include <deque>
#include <memory>
#include <stdexcept>

namespace pipetune {

template <typename T>
using GioObject = std::unique_ptr<T, decltype(&g_object_unref)>;

static constexpr auto snapshotAttributes =
    "standard::type,standard::size,standard::is-symlink,etag::value";

static std::string fileEtag(GFileInfo *info) {
  const auto *etag = g_file_info_get_etag(info);
  return etag != nullptr ? etag : "";
}

static std::size_t validateFileInfo(GFileInfo *info, std::size_t maximumBytes) {
  const auto size = g_file_info_get_size(info);
  // NOFOLLOW path queries report symlinks as a distinct file type. Stream
  // queries omit is-symlink, so checking that optional attribute is invalid.
  if (g_file_info_get_file_type(info) != G_FILE_TYPE_REGULAR || size < 0 ||
      static_cast<std::uint64_t>(size) > maximumBytes)
    throw std::runtime_error("asset must be a regular file within its byte budget");
  return static_cast<std::size_t>(size);
}

static cardio::promise<AssetFileInspection> readSource(
    const std::filesystem::path &path, std::size_t maximumBytes, std::size_t prefixBytes,
    bool hash, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const auto file = GioObject<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  const auto before = GioObject<GFileInfo>(co_await cardio::gio::query_info(
      file.get(), snapshotAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS), g_object_unref);
  const auto size = validateFileInfo(before.get(), maximumBytes);
  cancellation.throw_if_cancellation_requested();
  const auto stream = GioObject<GFileInputStream>(co_await cardio::gio::read(file.get()), g_object_unref);
  auto bytes = std::vector<std::uint8_t>(std::min(size, prefixBytes));
  auto scratch = std::vector<std::uint8_t>(size > bytes.size() ? 64 * 1024 : 0);
  const auto digest = std::unique_ptr<GChecksum, decltype(&g_checksum_free)>(
      hash ? g_checksum_new(G_CHECKSUM_SHA256) : nullptr, g_checksum_free);
  if (hash && !digest) throw std::bad_alloc();
  auto error = std::exception_ptr{};
  try {
    cancellation.throw_if_cancellation_requested();
    const auto opened = GioObject<GFileInfo>(co_await cardio::gio::query_info(
        stream.get(), snapshotAttributes), g_object_unref);
    if (validateFileInfo(opened.get(), maximumBytes) != size ||
        fileEtag(before.get()) != fileEtag(opened.get()))
      throw std::runtime_error("asset changed while opening");
    auto offset = std::size_t{0};
    while (offset < size) {
      cancellation.throw_if_cancellation_requested();
      auto *buffer = offset < bytes.size() ? bytes.data() + offset : scratch.data();
      const auto capacity = offset < bytes.size() ? bytes.size() - offset : scratch.size();
      const auto requested = std::min({capacity, size - offset, std::size_t{1024 * 1024}});
      const auto count = co_await cardio::gio::submit<gssize>(
          [input = G_INPUT_STREAM(stream.get()), buffer, requested](GCancellable *cancel,
              GAsyncReadyCallback callback, void *data) {
            g_input_stream_read_async(input, buffer, requested, G_PRIORITY_DEFAULT, cancel, callback, data);
          }, [](GObject *object, GAsyncResult *result, GError **error) {
            return g_input_stream_read_finish(G_INPUT_STREAM(object), result, error);
          }, cancellation);
      if (count <= 0) throw std::runtime_error("asset was truncated while reading");
      if (hash) g_checksum_update(digest.get(), buffer, count);
      offset += count;
    }
    auto extra = std::array<std::byte, 1>{};
    if ((co_await cardio::gio::read(G_INPUT_STREAM(stream.get()), std::span(extra))) != 0)
      throw std::runtime_error("asset grew while reading");
    const auto after = GioObject<GFileInfo>(co_await cardio::gio::query_info(
        file.get(), snapshotAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS), g_object_unref);
    if (validateFileInfo(after.get(), maximumBytes) != size ||
        fileEtag(before.get()) != fileEtag(after.get()))
      throw std::runtime_error("asset changed while reading");
  } catch (...) {
    error = std::current_exception();
  }
  co_await cardio::gio::close(G_INPUT_STREAM(stream.get()));
  if (error) std::rethrow_exception(error);
  cancellation.throw_if_cancellation_requested();
  co_return AssetFileInspection{{path, hash ? g_checksum_get_string(digest.get()) : "", size}, std::move(bytes)};
}

cardio::promise<std::vector<std::uint8_t>> readAssetFile(
    const std::filesystem::path &path, std::size_t maximumBytes, cardio::cancellation cancellation) {
  auto result = std::move(co_await readSource(path, maximumBytes, maximumBytes, false, cancellation));
  co_return std::move(result.prefix);
}

cardio::promise<AssetFileInspection> inspectAssetFile(const std::filesystem::path &path,
    std::size_t maximumBytes, std::size_t prefixBytes, cardio::cancellation cancellation) {
  co_return std::move(co_await readSource(path, maximumBytes, prefixBytes, true, cancellation));
}

static bool missingPath(const cardio::gio::gio_error &error) {
  return error.domain() == G_IO_ERROR && (error.code() == G_IO_ERROR_NOT_FOUND || error.code() == G_IO_ERROR_NOT_DIRECTORY);
}

static cardio::promise<std::optional<std::filesystem::path>> canonicalPath(
    const std::filesystem::path &path, std::vector<std::filesystem::path> &dependencies,
    cardio::cancellation cancellation) {
  auto pending = std::deque<std::filesystem::path>{};
  for (const auto &component : path.relative_path()) pending.push_back(component);
  auto resolved = path.root_path();
  auto links = 0u;
  while (!pending.empty()) {
    cancellation.throw_if_cancellation_requested();
    const auto component = std::move(pending.front());
    pending.pop_front();
    if (component.empty() || component == ".") continue;
    if (component == "..") { resolved = resolved.parent_path(); continue; }
    const auto next = resolved / component;
    const auto file = GioObject<GFile>(g_file_new_for_path(next.c_str()), g_object_unref);
    auto info = GioObject<GFileInfo>(nullptr, g_object_unref);
    try {
      info.reset(co_await cardio::gio::submit<GFileInfo *>(
          [file = file.get()](GCancellable *cancel, GAsyncReadyCallback callback, gpointer data) {
            g_file_query_info_async(file, "standard::type,standard::symlink-target",
                G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, G_PRIORITY_DEFAULT, cancel, callback, data);
          }, [](GObject *object, GAsyncResult *result, GError **error) {
            return g_file_query_info_finish(G_FILE(object), result, error);
          }, cancellation));
    } catch (const cardio::gio::gio_error &error) {
      if (!missingPath(error)) throw;
      dependencies.push_back(next);
      co_return std::nullopt;
    }
    if (g_file_info_get_file_type(info.get()) == G_FILE_TYPE_SYMBOLIC_LINK) {
      if (++links > 40) throw std::runtime_error("asset symbolic links are recursive or too deep");
      dependencies.push_back(next);
      if (!g_file_info_has_attribute(info.get(), G_FILE_ATTRIBUTE_STANDARD_SYMLINK_TARGET))
        throw std::runtime_error("asset symbolic link target is unavailable");
      const auto *target = g_file_info_get_symlink_target(info.get());
      if (!target || !*target) throw std::runtime_error("asset symbolic link target is invalid");
      const auto replacement = std::filesystem::path(target);
      if (replacement.is_absolute()) resolved = replacement.root_path();
      const auto relative = replacement.relative_path();
      const auto components = std::vector<std::filesystem::path>(relative.begin(), relative.end());
      pending.insert(pending.begin(), components.begin(), components.end());
    } else {
      if (!pending.empty() && g_file_info_get_file_type(info.get()) != G_FILE_TYPE_DIRECTORY) {
        dependencies.push_back(next);
        co_return std::nullopt;
      }
      resolved = next;
    }
  }
  co_return resolved;
}

cardio::promise<std::optional<std::filesystem::path>> resolveConfinedAssetPath(
    const std::filesystem::path &root, const std::filesystem::path &relativePath,
    std::vector<std::filesystem::path> &dependencies, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const auto text = relativePath.generic_string();
  if (!root.is_absolute() || relativePath.is_absolute() || text.empty() ||
      text.find('\0') != std::string::npos || text.find(':') != std::string::npos ||
      text.find('\\') != std::string::npos ||
      std::ranges::any_of(relativePath, [](const auto &part) { return part == ".."; }))
    throw std::runtime_error("invalid asset relative path");
  dependencies.push_back(root);
  dependencies.push_back(root / relativePath);
  const auto canonicalRoot = std::move(co_await canonicalPath(root, dependencies, cancellation));
  if (!canonicalRoot || *canonicalRoot != root) throw std::runtime_error("registered asset root changed or is unavailable");
  const auto target = std::move(co_await canonicalPath(root / relativePath, dependencies, cancellation));
  if (!target) co_return std::nullopt;
  const auto relative = target->lexically_relative(root);
  if (relative.empty() || relative == "." || relative.is_absolute() ||
      *relative.begin() == "..") throw std::runtime_error("asset dependency is outside its registered folder");
  dependencies.push_back(*target);
  co_return target;
}

std::string assetSha256(std::span<const std::uint8_t> bytes) {
  auto *digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes.data(), bytes.size());
  if (digest == nullptr) throw std::bad_alloc();
  const auto result = std::string(digest);
  g_free(digest);
  return result;
}

cardio::promise<void> verifyAssetSnapshots(std::span<const AssetFileSnapshot> snapshots,
                                         cardio::cancellation cancellation) {
  for (const auto &snapshot : snapshots) {
    auto path = snapshot.path;
    if (!snapshot.root.empty()) {
      auto dependencies = std::vector<std::filesystem::path>{};
      const auto resolved = std::move(co_await resolveConfinedAssetPath(snapshot.root,
          snapshot.path.lexically_relative(snapshot.root), dependencies, cancellation));
      if (snapshot.missing && !resolved) continue;
      if (snapshot.missing || !resolved || *resolved != snapshot.resolvedPath)
        throw std::runtime_error("asset source path changed during preparation: " + snapshot.path.string());
      path = *resolved;
    }
    const auto inspected = std::move(co_await inspectAssetFile(path, snapshot.bytes, 0, cancellation));
    if (inspected.snapshot.bytes != snapshot.bytes || inspected.snapshot.digest != snapshot.digest)
      throw std::runtime_error("asset source changed during preparation: " + snapshot.path.string());
  }
}

} // namespace pipetune
