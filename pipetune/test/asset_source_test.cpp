/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_file.h"
#include "measurement_store.h"
#include "preparation_dispatcher.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <functional>
#include <iostream>
#include <utility>
#include <unistd.h>

// Mutate only after the path and opened stream metadata have been checked.
// The wrapper is linked into this test, without changing GIO or production I/O.
static std::function<void()> beforeRead;
extern "C" void __real_g_input_stream_read_async(GInputStream *, void *, gsize,
    int, GCancellable *, GAsyncReadyCallback, gpointer);
extern "C" void __wrap_g_input_stream_read_async(GInputStream *stream, void *buffer,
    gsize count, int priority, GCancellable *cancellable, GAsyncReadyCallback callback, gpointer data) {
  if (beforeRead) std::exchange(beforeRead, {})();
  __real_g_input_stream_read_async(stream, buffer, count, priority, cancellable, callback, data);
}

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

static cardio::promise<void> testReadChanges(const std::filesystem::path &directory) {
  using namespace pipetune;
  const auto path = directory / "changing.bin";
  const auto replacement = directory / "replacement.bin";
  // Grow past the limit, grow within it, replace the path, grow the opened file
  // and replace its path, cancel during I/O, and grow from the exact limit.
  for (auto scenario = 0; scenario < 6; ++scenario) {
    std::ofstream(path, std::ios::binary) << std::string(scenario == 5 ? 16 : 8, 'a');
    auto cancellation = cardio::cancellation_source{};
    beforeRead = [&] {
      if (scenario == 4) { cancellation.cancel(); return; }
      if (scenario != 2) std::filesystem::resize_file(path, scenario == 1 ? 9 : 17);
      if (scenario == 2 || scenario == 3) {
        std::ofstream(replacement, std::ios::binary) << std::string(scenario == 2 ? 9 : 4, 'b');
        std::filesystem::rename(replacement, path);
      }
    };
    auto oversized = false, changed = false, canceled = false;
    try { static_cast<void>(co_await readAssetFile(path, 16, cancellation.get_cancellation())); }
    catch (const AssetFileSizeError &) { oversized = true; }
    catch (const cardio::canceled_exception &) { canceled = true; }
    catch (const std::runtime_error &) { changed = true; }
    require(!beforeRead, "the mutation must run at the first read boundary");
    require(oversized == (scenario == 0 || scenario == 3 || scenario == 5),
        "growth beyond the limit must retain its size-error classification, including a replaced path");
    require(changed == (scenario == 1 || scenario == 2), "in-budget growth and replacement must reject the candidate as changed");
    require(canceled == (scenario == 4), "cancellation at the read boundary must reject the candidate as canceled");
    const auto recovered = std::move(co_await readAssetFile(path, 64, {}));
    require(recovered.size() == std::filesystem::file_size(path) &&
        recovered.front() == (scenario == 2 || scenario == 3 ? 'b' : 'a'),
        "a fresh read must recover from the current valid file after rejection");
  }
  std::ofstream(path, std::ios::binary) << std::string(16, 'x');
  require((co_await readAssetFile(path, 16, {})).size() == 16, "the exact byte limit must be accepted");

  const auto left = directory / "ear-l.json";
  std::ofstream(left) << "{}";
  auto store = MeasurementStore{directory, {}};
  const auto ids = std::array<std::string, 4>{"ear-l::ch=left", "ear-r::ch=left", "ear-l::ch=right", "ear-r::ch=right"};
  beforeRead = [&] { std::filesystem::resize_file(left, 64u * 1024u * 1024u + 1u); };
  const auto measurements = std::move(co_await loadCrosstalkMeasurementsAsync(store, ids, {}));
  require(!beforeRead && measurements.exceedsByteBudget && !measurements.error.empty(),
      "growth during Crosstalk loading must reject preparation rather than omit the processor");
  require(measurements.files.size() == 2 && store.documents.empty() && store.snapshots.empty(),
      "a rejected measurement must retain dependencies without publishing partial cache entries");
}

static cardio::promise<bool> testSources(const std::filesystem::path &directory) {
  using namespace pipetune;
  const auto root = directory / "instrument";
  std::filesystem::create_directories(root / "samples");
  const auto original = root / "samples" / "a.wav";
  const auto bytes = std::vector<std::uint8_t>(3 * 1024 * 1024, 42);
  std::ofstream(original, std::ios::binary).write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  const auto inspected = std::move(co_await inspectAssetFile(original, bytes.size(), 64, {}));
  require(inspected.prefix == std::vector<std::uint8_t>(64, 42) && inspected.snapshot.bytes == bytes.size() &&
      inspected.snapshot.digest == assetSha256(bytes), "bounded inspection must hash the complete source and retain only its prefix");
  std::filesystem::create_symlink("samples", root / "alias");
  auto dependencies = std::vector<std::filesystem::path>{};
  const auto resolved = std::move(co_await resolveConfinedAssetPath(root, "alias/a.wav", dependencies, {}));
  require(resolved && *resolved == original && std::ranges::find(dependencies, root / "alias") != dependencies.end(),
      "an in-root symbolic link must resolve and remain a watched dependency");
  auto snapshot = inspected.snapshot;
  snapshot.path = root / "alias" / "a.wav";
  snapshot.root = root;
  snapshot.resolvedPath = original;
  const auto snapshots = std::array{snapshot};
  co_await verifyAssetSnapshots(snapshots, {});
  std::fstream changed(original, std::ios::in | std::ios::out | std::ios::binary);
  changed.seekp(-1, std::ios::end); changed.put('!'); changed.close();
  auto rejected = false;
  try { co_await verifyAssetSnapshots(snapshots, {}); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "changes beyond the retained prefix must invalidate preparation");
  const auto outside = directory / "outside";
  std::filesystem::create_directories(outside);
  std::ofstream(outside / "a.wav") << "outside";
  std::filesystem::remove(root / "alias");
  std::filesystem::create_directory_symlink(outside, root / "alias");
  rejected = false;
  try { static_cast<void>(co_await resolveConfinedAssetPath(root, "alias/a.wav", dependencies, {})); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "an out-of-root link must not be accepted as a sample source");
  const auto missing = std::move(co_await resolveConfinedAssetPath(root, "absent.wav", dependencies, {}));
  require(!missing, "a missing sample must remain distinguishable from invalid or unreadable data");
  const auto absent = std::array{AssetFileSnapshot{.path = root / "absent.wav", .digest = {}, .bytes = 0,
      .root = root, .resolvedPath = {}, .missing = true}};
  co_await verifyAssetSnapshots(absent, {});
  std::ofstream(root / "absent.wav") << "appeared";
  rejected = false;
  try { co_await verifyAssetSnapshots(absent, {}); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "a newly available omitted sample must invalidate the candidate snapshot");
  std::filesystem::rename(root, directory / "moved");
  std::filesystem::create_directory_symlink(directory / "moved", root);
  rejected = false;
  try { static_cast<void>(co_await resolveConfinedAssetPath(root, "samples/a.wav", dependencies, {})); }
  catch (const std::runtime_error &) { rejected = true; }
  require(rejected, "a registered canonical root must not silently follow a replacement link");
  auto cancellation = cardio::cancellation_source{};
  cancellation.cancel();
  auto canceled = false;
  try { static_cast<void>(co_await inspectAssetFile(original, bytes.size(), 64, cancellation.get_cancellation())); }
  catch (const cardio::canceled_exception &) { canceled = true; }
  require(canceled, "cancellation must be observed before touching a source path");
  co_await testReadChanges(directory);
  co_return true;
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() / ("pipetune-source-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  auto success = false;
  try { success = pipetune::runPreparation<bool>([&] { return testSources(directory); }); }
  catch (const std::exception &error) { std::cerr << error.what() << '\n'; }
  std::filesystem::remove_all(directory);
  return success ? 0 : 1;
}
