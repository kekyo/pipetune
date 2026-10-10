/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "asset_file.h"
#include "preparation_dispatcher.h"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <unistd.h>

static void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
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
