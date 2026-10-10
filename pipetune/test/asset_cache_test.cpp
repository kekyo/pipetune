/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_cache.h"
#include "ir/preparation.h"
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <unistd.h>

static std::atomic<bool> measureAllocations{false};
static std::atomic<std::size_t> measuredBytes{0};
static std::atomic<std::size_t> peakBytes{0};

struct alignas(std::max_align_t) AllocationHeader { std::size_t measured; };

void *operator new(std::size_t bytes) {
  auto *header = static_cast<AllocationHeader *>(std::malloc(sizeof(AllocationHeader) + std::max(bytes, std::size_t{1})));
  if (!header) throw std::bad_alloc();
  header->measured = measureAllocations.load() ? bytes : 0;
  if (header->measured != 0) {
    const auto live = measuredBytes.fetch_add(bytes) + bytes;
    auto peak = peakBytes.load();
    while (peak < live && !peakBytes.compare_exchange_weak(peak, live)) {}
  }
  return header + 1;
}

void operator delete(void *pointer) noexcept {
  if (!pointer) return;
  auto *header = static_cast<AllocationHeader *>(pointer) - 1;
  if (header->measured != 0) measuredBytes.fetch_sub(header->measured);
  std::free(header);
}
void *operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete[](void *pointer) noexcept { ::operator delete(pointer); }
void operator delete(void *pointer, std::size_t) noexcept { ::operator delete(pointer); }
void operator delete[](void *pointer, std::size_t) noexcept { ::operator delete(pointer); }

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static pipetune::PreparedDspAsset makeImpulse(std::size_t frames = 512) {
  auto audio = pipetune::assets::Audio{48000, {std::vector<float>(frames)}};
  audio.channels[0][0] = 1;
  audio.channels[0][64] = 0.5F;
  const auto config = pipetune::assets::resolveIrConfiguration(48000, 1, 2, "mono", 128, "full");
  auto prepared = pipetune::assets::prepareIr(audio, config, {}).asset;
  auto asset = pipetune::PreparedDspAsset{};
  asset.info = {.channels = prepared.channels, .frames = prepared.frames,
      .topology = static_cast<std::uint32_t>(prepared.config.topology),
      .head_block = prepared.config.headBlock, .rate_divider = prepared.config.rateDivider,
      .path_count = static_cast<std::uint32_t>(prepared.config.paths.size()),
      .input_count = prepared.inputCount, .processing_channels = 2,
      .footprint_bytes = static_cast<std::uint32_t>(prepared.footprintBytes),
      .byte_size = static_cast<std::uint32_t>(prepared.payload.size())};
  asset.payload = std::move(prepared.payload);
  asset.warmupFrames = 37;
  return asset;
}

static cardio::promise<void> testCache(const std::filesystem::path &directory) {
  const auto options = pipetune::AssetCacheOptions{directory / "cache", 1024 * 1024};
  const auto keyA = std::string(64, 'a');
  const auto keyB = std::string(64, 'b');
  const auto keyC = std::string(64, 'c');
  const auto asset = makeImpulse();
  const auto diagnostics = std::vector<std::string>{"Retained preparation condition"};
  require(!(co_await pipetune::readAssetCache(options, keyA, 32 * 1024 * 1024)), "a cold cache must be a miss");
  auto saved = co_await pipetune::writeAssetCache(options, keyA, asset, diagnostics);
  require(saved.error.empty() && saved.pin != nullptr, "a valid prepared IR must be persisted");
  auto cached = co_await pipetune::readAssetCache(options, keyA, 32 * 1024 * 1024);
  require(cached && cached->asset.payload == asset.payload &&
      cached->asset.info.footprint_bytes == asset.info.footprint_bytes &&
      cached->asset.info.processing_channels == 2 && cached->asset.warmupFrames == asset.warmupFrames &&
      cached->diagnostics == diagnostics,
      "a cache hit must preserve the IR coefficients, copy contract, and diagnostics");
  require(!(co_await pipetune::readAssetCache(options, keyB, 32 * 1024 * 1024)), "a different source identity must miss");
  require(!(co_await pipetune::readAssetCache(options, keyA, 1)), "a cache hit must obey the caller's payload budget");

  const auto entryPath = saved.pin->path;
  auto corrupt = std::fstream(entryPath, std::ios::in | std::ios::out | std::ios::binary);
  corrupt.seekp(-1, std::ios::end);
  corrupt.put('\x7f');
  corrupt.close();
  require(!(co_await pipetune::readAssetCache(options, keyA, 32 * 1024 * 1024)), "damaged coefficients must never become a cache hit");
  saved = co_await pipetune::writeAssetCache(options, keyA, asset, diagnostics);
  auto recovered = co_await pipetune::readAssetCache(options, keyA, 32 * 1024 * 1024);
  require(saved.error.empty() && recovered && recovered->asset.payload == asset.payload,
          "a corrupt entry must be replaceable with regenerated IR data");
  recovered.reset();
  cached.reset();
  saved.pin.reset();

  const auto entryBytes = std::filesystem::file_size(entryPath);
  const auto concurrentOptions = pipetune::AssetCacheOptions{directory / "concurrent", entryBytes};
  std::filesystem::create_directories(concurrentOptions.directory / "assets-v1");
  const auto concurrentKeys = std::array{std::string(64, '1'), std::string(64, '2'), std::string(64, '3')};
  auto pendingWrites = std::vector<cardio::promise<pipetune::AssetCacheWriteResult>>{};
  for (const auto &key : concurrentKeys)
    pendingWrites.push_back(pipetune::writeAssetCache(concurrentOptions, key, asset, diagnostics));
  auto persisted = std::vector<pipetune::AssetCacheWriteResult>{};
  for (auto &pending : pendingWrites) persisted.push_back(std::move(co_await pending));
  auto diskBytes = std::uint64_t{0};
  for (const auto &entry : std::filesystem::directory_iterator(concurrentOptions.directory / "assets-v1"))
    diskBytes += entry.file_size();
  require(diskBytes <= entryBytes && std::ranges::count_if(persisted,
      [](const auto &result) { return result.pin != nullptr; }) == 1,
      "concurrent writes must not oversubscribe a cache that holds one pinned asset");
  const auto limited = pipetune::AssetCacheOptions{options.directory, entryBytes * 2};
  auto second = co_await pipetune::writeAssetCache(limited, keyB, asset, diagnostics);
  require(second.error.empty(), "two cache entries must fit their exact disk budget");
  // Give the two entries an explicit age without sleeping; a cache hit must
  // refresh A so that the newer write C evicts B rather than the reused A.
  std::filesystem::last_write_time(entryPath, std::filesystem::file_time_type::clock::now() - std::chrono::hours(48));
  std::filesystem::last_write_time(second.pin->path, std::filesystem::file_time_type::clock::now() - std::chrono::hours(24));
  second.pin.reset();
  auto reused = co_await pipetune::readAssetCache(limited, keyA, 32 * 1024 * 1024);
  require(reused.has_value(), "the existing entry must remain reusable");
  reused.reset();
  auto third = co_await pipetune::writeAssetCache(limited, keyC, asset, diagnostics);
  require(third.error.empty(), "the cache must reclaim an unused entry before writing");
  require(!(co_await pipetune::readAssetCache(limited, keyB, 32 * 1024 * 1024)), "the least recently used entry must be evicted");
  auto first = co_await pipetune::readAssetCache(limited, keyA, 32 * 1024 * 1024);
  require(first && first->asset.payload == asset.payload, "recently reused coefficients must survive eviction");
  const auto blocked = co_await pipetune::writeAssetCache(limited, keyB, asset, diagnostics);
  require(!blocked.error.empty() && first->asset.payload == asset.payload && third.pin != nullptr,
          "a full pinned cache must preserve active assets and report a persistence failure");

  std::ofstream(directory / "not-a-directory") << "file";
  const auto unavailable = co_await pipetune::writeAssetCache(
      {directory / "not-a-directory", 1024 * 1024}, keyA, asset, diagnostics);
  require(!unavailable.error.empty() && asset.payload == makeImpulse().payload,
          "a cache write failure must leave the prepared IR usable in memory");

  const auto largeAsset = makeImpulse(1024 * 1024);
  const auto largeOptions = pipetune::AssetCacheOptions{directory / "large", 16 * 1024 * 1024};
  const auto largeSaved = co_await pipetune::writeAssetCache(largeOptions, keyA, largeAsset, {});
  require(largeSaved.error.empty(), "the large prepared IR must be cached before measuring reuse");
  measureAllocations.store(true);
  auto largeRead = std::move(co_await pipetune::readAssetCache(largeOptions, keyA, 32 * 1024 * 1024));
  measureAllocations.store(false);
  require(largeRead && largeRead->asset.payload == largeAsset.payload,
          "bounded cache reuse must preserve the complete IR");
  std::cout << "cached_payload_bytes=" << largeAsset.payload.size() << " peak_temporary_cpp_bytes=" << peakBytes.load() << '\n';
  require(peakBytes.load() <= largeAsset.payload.size() + 256 * 1024,
          "cache reuse must not require a second bank-sized C++ allocation");
}

static cardio::promise<void> run(cardio::dispatcher_group_glib &group,
                                 const std::filesystem::path &directory, bool &success) {
  try { co_await testCache(directory); success = true; }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  group.shutdown();
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() / ("pipetune-cache-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto context = std::unique_ptr<GMainContext, decltype(&g_main_context_unref)>(g_main_context_new(), g_main_context_unref);
  auto success = false;
  g_main_context_push_thread_default(context.get());
  {
    cardio::dispatcher_group_glib group(context.get());
    cardio::dispatcher_host_glib host(group);
    const auto operation = run(group, directory, success);
    host.park();
  }
  g_main_context_pop_thread_default(context.get());
  std::filesystem::remove_all(directory);
  return success ? 0 : 1;
}
