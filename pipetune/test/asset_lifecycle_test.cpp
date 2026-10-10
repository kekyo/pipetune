/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pipetune/control_protocol.h>
#include <pipetune/dsp_pipeline.h>
#include "preparation_generation.h"
#include "asset_file.h"
#include "ir_asset_loader.h"
#include "preparation_dispatcher.h"
#include "preparation_termination.h"

#include <algorithm>
#include <bit>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/eventfd.h>
#include <sys/wait.h>
#include <unistd.h>

static bool check(bool condition, const char *message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static bool testPreparationDiagnostics(const std::filesystem::path &directory) {
  const auto preset = directory / "empty.effetune_preset";
  std::ofstream(preset) << R"({"pipeline":[{"name":"IR Reverb","parameters":{}}]})";
  const auto loaded = pipetune::loadDspPipeline(preset, {48000, 2, 128},
      {.measurementDirectory = directory / "measurements", .effetuneDirectory = directory});
  if (!check(loaded.pipeline != nullptr, loaded.error.c_str())) return false;
  const auto entries = loaded.pipeline->presetEntries();
  return check(loaded.pipeline->activePluginCount() == 1 &&
                   entries.size() == 1 && entries[0].state == pipetune::PresetEntryState::enabled,
               "an unassigned IR must remain an active unloaded DSP") &&
         check(loaded.warnings.size() == 1 &&
                   loaded.warnings[0].state == pipetune::PresetEntryState::enabled &&
                   entries[0].diagnostics == std::vector<std::string>{loaded.warnings[0].reason},
               "an active unassigned IR must retain its preparation diagnostic");
}

static bool testFailedDependencies(const std::filesystem::path &directory) {
  const auto preset = directory / "missing.effetune_preset";
  std::ofstream(preset) << R"({"pipeline":[{"name":"IR Reverb","parameters":{"ir":"000000000000000000000000"}}]})";
  const auto context = pipetune::PipelineLoadContext{
      .measurementDirectory = directory / "measurements", .effetuneDirectory = directory};
  const auto index = directory / "ir-library" / "index.json";
  const auto original = directory / "ir-library" / "000000000000000000000000.wav";
  const auto missingIndex = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  const auto indexRetained = check(missingIndex.pipeline == nullptr && !missingIndex.error.empty() &&
      missingIndex.dependencyFiles == std::vector<std::filesystem::path>{index},
      "a failed load must retain the missing registry for recovery monitoring");
  std::filesystem::create_directories(index.parent_path());
  std::ofstream(index) << R"({"version":1,"entries":{"000000000000000000000000":{"irId":"000000000000000000000000","composition":"single","originals":[{"role":"single","fileName":"missing.wav","storageName":"000000000000000000000000.wav","sha256":"0000000000000000000000000000000000000000000000000000000000000000","byteLength":44}],"bytes":44}}})";
  const auto missingOriginal = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  const auto sourceRetained = check(missingOriginal.pipeline == nullptr && !missingOriginal.error.empty() &&
      missingOriginal.dependencyFiles == std::vector<std::filesystem::path>{index, original},
      "a failed load must retain both the registry and missing original");
  std::ofstream(preset) << R"({"pipeline":[{"name":"IR Reverb","enabled":false,"parameters":{"ir":"000000000000000000000000"}}]})";
  const auto disabled = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  return check(disabled.pipeline != nullptr && disabled.warnings.empty() && disabled.dependencyFiles.empty(),
               "a disabled IR must not resolve or observe its missing original") && indexRetained && sourceRetained;
}

static bool testPublishedDiagnostics() {
  auto status = pipetune::ControlRuntimeStatus{};
  status.processingMode = pipetune::ProcessingMode::preset;
  status.dspActivity = pipetune::DspActivity::active;
  status.activePreset = "/tmp/diagnostics.effetune_preset";
  status.dspSampleRate = 48000;
  status.activePluginCount = 1;
  status.presetEntries = {{"IR Reverb", pipetune::PresetEntryState::enabled, {"No IR is assigned"}}};
  const auto warnings = std::vector<pipetune::ControlWarning>{
      {0, "IR Reverb", "No IR is assigned", pipetune::PresetEntryState::enabled}};
  auto correct = true;
  for (const auto event : {false, true}) {
    const auto encoded = event ? pipetune::makeControlStatusEvent(status) :
                                 pipetune::makeControlSuccessResponse(status, warnings);
    const auto decoded = pipetune::parseControlResponse(encoded);
    correct = check(decoded.valid && decoded.status.presetEntries == status.presetEntries,
                    "status replies and publications must retain active preparation diagnostics") && correct;
    if (!event) correct = check(decoded.warnings.size() == 1 &&
        decoded.warnings[0].state == pipetune::PresetEntryState::enabled,
        "control replies must distinguish active warnings from omitted nodes") && correct;
  }
  return correct;
}

static std::filesystem::path writeSharedIrPreset(const std::filesystem::path &directory) {
  auto wave = std::vector<std::uint8_t>(44 + 512 * sizeof(float));
  const auto put = [&wave](std::size_t offset, std::uint32_t value, std::size_t count) {
    for (auto byte = std::size_t{0}; byte < count; ++byte)
      wave[offset + byte] = static_cast<std::uint8_t>(value >> (8 * byte));
  };
  for (const auto &[offset, text] : std::array{std::pair{0u, "RIFF"}, std::pair{8u, "WAVE"},
                                           std::pair{12u, "fmt "}, std::pair{36u, "data"}})
    std::copy_n(text, 4, wave.begin() + offset);
  put(4, wave.size() - 8, 4); put(16, 16, 4); put(20, 3, 2); put(22, 1, 2);
  put(24, 48000, 4); put(28, 48000 * 4, 4); put(32, 4, 2); put(34, 32, 2);
  put(40, wave.size() - 44, 4); put(44, std::bit_cast<std::uint32_t>(1.0F), 4);
  const auto digest = pipetune::assetSha256(wave);
  const auto id = digest.substr(0, 24);
  const auto library = directory / "ir-library";
  std::filesystem::create_directories(library);
  std::ofstream(library / (id + ".wav"), std::ios::binary).write(
      reinterpret_cast<const char *>(wave.data()), wave.size());
  std::ofstream(library / "index.json") << "{\"version\":1,\"entries\":{\"" << id <<
      "\":{\"irId\":\"" << id << "\",\"composition\":\"single\",\"bytes\":" << wave.size() <<
      ",\"originals\":[{\"role\":\"single\",\"fileName\":\"impulse.wav\",\"storageName\":\"" << id <<
      ".wav\",\"sha256\":\"" << digest << "\",\"byteLength\":" << wave.size() << "}]}}}";
  const auto preset = directory / "shared.effetune_preset";
  const auto node = "{\"name\":\"IR Reverb\",\"parameters\":{\"ir\":\"" + id + "\"}}";
  std::ofstream(preset) << "{\"pipeline\":[" << node << ',' << node << "]}";
  return preset;
}

static bool testSharedPreparation(const std::filesystem::path &directory) {
  const auto preset = writeSharedIrPreset(directory);
  const auto context = pipetune::PipelineLoadContext{directory / "measurements", directory};
  auto loaded = pipetune::loadDspPipeline(preset, {48000, 2, 128}, context);
  if (!check(loaded.pipeline != nullptr, loaded.error.c_str())) return false;
  auto limited = context;
  limited.assetMemoryBytes = 1;
  const auto rejected = pipetune::loadDspPipeline(preset, {48000, 2, 128}, limited);
  const auto bounded = check(!rejected.pipeline && rejected.error.find("memory budget") != std::string::npos,
      "an asset must be rejected before preparation exceeds the combined memory budget");
  auto impulse = std::vector<float>(256);
  impulse[0] = 1;
  return check(loaded.pipeline->process(impulse, 2, 128, 0) == pipetune::ProcessStatus::ok &&
          loaded.pipeline->latencyFrames() == 256,
          "rejecting a replacement must retain the old pipeline and its latency") &&
      check(loaded.pipeline->activePluginCount() == 2 && loaded.pipeline->latencyFrames() == 256 &&
          loaded.cachedAssetCount == 1,
          "identical nodes must share prepared coefficients even when persistent caching is disabled") && bounded;
}

static bool testCombinedReservations() {
  auto previous = pipetune::AssetMemoryReservation(70, 100);
  auto rejected = false;
  try { const auto next = pipetune::AssetMemoryReservation(31, 100); }
  catch (const std::length_error &) { rejected = true; }
  if (!check(rejected, "old and new assets must consume the same process memory budget")) return false;
  auto retained = std::move(previous);
  retained.shrink(60);
  {
    const auto exact = pipetune::AssetMemoryReservation(40, 100);
    rejected = false;
    try { const auto overflow = pipetune::AssetMemoryReservation(1, 100); }
    catch (const std::length_error &) { rejected = true; }
    if (!check(rejected, "an exact-fit reservation must exclude any additional asset")) return false;
  }
  retained.shrink(0);
  const auto released = pipetune::AssetMemoryReservation(100, 100);
  rejected = false;
  try { const auto overflow = pipetune::AssetMemoryReservation(UINT64_MAX, UINT64_MAX); }
  catch (const std::length_error &) { rejected = true; }
  return check(rejected, "oversized reservations must reject before integer overflow or allocation");
}

static cardio::promise<void> testAsyncPreparation(cardio::dispatcher_group_glib &group,
    const std::filesystem::path &directory, bool &correct) {
  const auto backend = pipetune::loadDspBackend(pipetune::DspBackendKind::scalar);
  const auto context = pipetune::PipelineLoadContext{directory / "measurements", directory};
  const auto preset = directory / "empty.effetune_preset";
  auto generations = pipetune::PreparationGeneration{};
  const auto older = generations.begin();
  auto cancellation = older.source;
  auto pending = pipetune::loadDspPipelineAsync(preset, {48000, 2, 128}, backend.backend,
                                              context, cancellation.get_cancellation());
  correct = check(pending.try_result() == nullptr, "preparation must yield before completing native construction") && correct;
  const auto newer = generations.begin();
  correct = check(!generations.current(older) && generations.current(newer),
                  "new configuration intent did not invalidate the older candidate") && correct;
  auto canceled = false;
  try { static_cast<void>(co_await pending); }
  catch (const cardio::canceled_exception &) { canceled = true; }
  correct = check(canceled, "superseded preparation must be canceled before publication") && correct;
  auto preCanceled = false;
  try {
    static_cast<void>(co_await pipetune::loadDspPipelineAsync(directory / "never-open.effetune_preset",
        {48000, 2, 128}, backend.backend, context, cancellation.get_cancellation()));
  } catch (const cardio::canceled_exception &) { preCanceled = true; }
  correct = check(preCanceled, "an already canceled request must not attempt to open its preset") && correct;
  auto loaded = std::move(co_await pipetune::loadDspPipelineAsync(
      preset, {48000, 2, 128}, backend.backend, context, {}));
  correct = check(loaded.pipeline != nullptr && loaded.pipeline->activePluginCount() == 1 &&
      loaded.warnings.size() == 1, "asynchronous preparation lost the IR node or its diagnostics") && correct;
  if (loaded.pipeline) {
    auto rebuilding = pipetune::rebuildDspPipelineAsync(*loaded.pipeline, {96000, 2, 128}, {}, {});
    loaded.pipeline.reset();
    auto rebuilt = std::move(co_await rebuilding);
    correct = check(rebuilt.pipeline != nullptr && rebuilt.pipeline->sampleRate() == 96000 &&
        rebuilt.pipeline->activePluginCount() == 1,
        "a rebuild retained a dependency on the retired source engine") && correct;
  }
  generations.invalidate();
  correct = check(!generations.current(newer), "shutdown left a candidate publishable") && correct;
  generations.shutdown();
  const auto afterShutdown = generations.begin();
  correct = check(!generations.current(afterShutdown) && afterShutdown.source.get_cancellation().is_cancellation_requested(),
      "shutdown must reject new preparation as well as invalidate older candidates") && correct;

  const auto sharedPreset = directory / "shared.effetune_preset";
  auto input = std::ifstream(sharedPreset);
  const auto json = std::string(std::istreambuf_iterator<char>(input), {});
  const auto document = std::unique_ptr<yyjson_doc, decltype(&yyjson_doc_free)>(
      yyjson_read(json.data(), json.size(), 0), yyjson_doc_free);
  auto *parameters = yyjson_obj_get(yyjson_arr_get(yyjson_obj_get(yyjson_doc_get_root(document.get()),
      "pipeline"), 0), "parameters");
  auto waitingCancellation = cardio::cancellation_source{};
  const auto cache = pipetune::AssetCacheOptions{};
  auto first = pipetune::loadIrAssetAsync(directory, parameters, 48000, 2, cache, {}, pipetune::kDefaultAssetMemoryBytes);
  auto waiting = pipetune::loadIrAssetAsync(directory, parameters, 48000, 2, cache,
      waitingCancellation.get_cancellation(), pipetune::kDefaultAssetMemoryBytes);
  auto same = pipetune::loadIrAssetAsync(directory, parameters, 48000, 2, cache, {}, pipetune::kDefaultAssetMemoryBytes);
  waitingCancellation.cancel();
  auto waitingCanceled = false;
  try { static_cast<void>(co_await waiting); }
  catch (const cardio::canceled_exception &) { waitingCanceled = true; }
  const auto prepared = std::move(co_await first);
  const auto reused = std::move(co_await same);
  correct = check(waitingCanceled && prepared.error.empty() && reused.error.empty() &&
      prepared.prepared != nullptr && prepared.prepared == reused.prepared && reused.cacheHit,
      "canceling one queued load must preserve a shared immutable result for the other requests") && correct;

  // A second dispatcher models the PipeWire main loop rebuilding while the
  // control service retains preparation results. No promise crosses groups.
  auto completion = cardio::promise_source<pipetune::IrAssetLoadResult>{};
  auto worker = std::thread([&] {
    auto other = pipetune::loadIrAsset(directory, parameters, 48000, 2, cache);
    completion.try_resolve(std::move(other));
  });
  const auto acrossDispatchers = std::move(co_await completion.get_promise());
  worker.join();
  correct = check(acrossDispatchers.prepared == prepared.prepared && acrossDispatchers.cacheHit,
      "prepared ownership must be reusable across dispatcher groups without copying its payload") && correct;
  co_await pipetune::verifyAssetSnapshots(prepared.snapshots, {});
  const auto source = prepared.snapshots.front().path;
  const auto original = std::move(co_await pipetune::readAssetFile(source, 64 * 1024 * 1024));
  std::ofstream(source, std::ios::binary) << "changed after preparation";
  auto changed = false;
  try { co_await pipetune::verifyAssetSnapshots(prepared.snapshots, {}); }
  catch (const std::runtime_error &) { changed = true; }
  correct = check(changed, "a completed asset must not publish after an earlier source snapshot changes") && correct;
  std::ofstream(source, std::ios::binary).write(reinterpret_cast<const char *>(original.data()), original.size());
  group.shutdown();
}

static cardio::promise<int> waitForPreparationCancellation(int ready, cardio::cancellation cancellation) {
  const auto descriptor = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (descriptor < 0) throw std::runtime_error("cannot create preparation barrier");
  static_cast<void>(write(ready, "R", 1));
  auto failure = std::exception_ptr{};
  try { co_await cardio::from_fd(descriptor, cardio::fd_event::read, cancellation); }
  catch (...) { failure = std::current_exception(); }
  close(descriptor);
  if (failure) std::rethrow_exception(failure);
  co_return 1;
}

static int runTerminationChild(int ready) {
  auto signals = sigset_t{};
  sigemptyset(&signals); sigaddset(&signals, SIGINT); sigaddset(&signals, SIGTERM);
  if (pthread_sigmask(SIG_BLOCK, &signals, nullptr) != 0) return 1;
  auto interrupted = false;
  auto canceled = false;
  try {
    static_cast<void>(pipetune::runPreparation<int>([&] {
      return pipetune::withPreparationTermination<int>([&](cardio::cancellation cancellation) {
        return waitForPreparationCancellation(ready, cancellation);
      }, {}, interrupted);
    }));
  } catch (const cardio::canceled_exception &) { canceled = true; }
  return interrupted && canceled ? 0 : 1;
}

static bool testPreparationTermination() {
  auto descriptors = std::array<int, 2>{};
  if (pipe(descriptors.data()) != 0) return false;
  const auto ready = std::to_string(descriptors[1]);
  const auto child = fork();
  if (child == 0) {
    close(descriptors[0]);
    execl("/proc/self/exe", "pipetune_asset_lifecycle_tests", "--termination-child", ready.c_str(), nullptr);
    _exit(127);
  }
  close(descriptors[1]);
  if (child < 0) { close(descriptors[0]); return false; }
  auto notification = char{};
  const auto entered = read(descriptors[0], &notification, 1) == 1 && notification == 'R';
  close(descriptors[0]);
  kill(child, entered ? SIGTERM : SIGKILL);
  auto status = 0;
  waitpid(child, &status, 0);
  return check(entered && WIFEXITED(status) && WEXITSTATUS(status) == 0,
      "termination must cancel and drain a suspended preparation without a running PipeWire loop");
}

int main(int argc, char **argv) {
  if (argc == 3 && std::string_view(argv[1]) == "--termination-child") return runTerminationChild(std::stoi(argv[2]));
  if (!testPreparationTermination()) return 1;
  const auto directory = std::filesystem::temp_directory_path() /
      ("pipetune-asset-lifecycle-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto diagnostics = testPreparationDiagnostics(directory);
  const auto dependencies = testFailedDependencies(directory);
  const auto publication = testPublishedDiagnostics();
  const auto sharing = testSharedPreparation(directory);
  const auto reservations = testCombinedReservations();
  auto asynchronous = true;
  auto *context = g_main_context_new();
  g_main_context_push_thread_default(context);
  {
    cardio::dispatcher_group_glib group(context);
    cardio::dispatcher_host_glib host(group);
    const auto operation = testAsyncPreparation(group, directory, asynchronous);
    host.park();
  }
  g_main_context_pop_thread_default(context);
  g_main_context_unref(context);
  std::filesystem::remove_all(directory);
  return diagnostics && dependencies && publication && sharing && reservations && asynchronous ? 0 : 1;
}
