/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_file.h"

#include <array>
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

cardio::promise<std::vector<std::uint8_t>> readAssetFile(
    const std::filesystem::path &path, std::size_t maximumBytes, cardio::cancellation cancellation) {
  cancellation.throw_if_cancellation_requested();
  const auto file = GioObject<GFile>(g_file_new_for_path(path.c_str()), g_object_unref);
  const auto before = GioObject<GFileInfo>(co_await cardio::gio::query_info(
      file.get(), snapshotAttributes, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS), g_object_unref);
  const auto size = validateFileInfo(before.get(), maximumBytes);
  cancellation.throw_if_cancellation_requested();
  const auto stream = GioObject<GFileInputStream>(co_await cardio::gio::read(file.get()), g_object_unref);
  auto bytes = std::vector<std::uint8_t>(size);
  auto error = std::exception_ptr{};
  try {
    cancellation.throw_if_cancellation_requested();
    const auto opened = GioObject<GFileInfo>(co_await cardio::gio::query_info(
        stream.get(), snapshotAttributes), g_object_unref);
    if (validateFileInfo(opened.get(), maximumBytes) != size ||
        fileEtag(before.get()) != fileEtag(opened.get()))
      throw std::runtime_error("asset changed while opening");
    auto offset = std::size_t{0};
    while (offset < bytes.size()) {
      cancellation.throw_if_cancellation_requested();
      const auto count = co_await cardio::gio::submit<gssize>(
          [input = G_INPUT_STREAM(stream.get()), &bytes, offset](GCancellable *cancel,
              GAsyncReadyCallback callback, void *data) {
            g_input_stream_read_async(input, bytes.data() + offset,
                std::min<std::size_t>(bytes.size() - offset, 1024u * 1024u),
                G_PRIORITY_DEFAULT, cancel, callback, data);
          }, [](GObject *object, GAsyncResult *result, GError **error) {
            return g_input_stream_read_finish(G_INPUT_STREAM(object), result, error);
          }, cancellation);
      if (count <= 0) throw std::runtime_error("asset was truncated while reading");
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
  co_return bytes;
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
    const auto bytes = std::move(co_await readAssetFile(snapshot.path, snapshot.bytes, cancellation));
    if (bytes.size() != snapshot.bytes || assetSha256(bytes) != snapshot.digest)
      throw std::runtime_error("asset source changed during preparation: " + snapshot.path.string());
  }
}

} // namespace pipetune
