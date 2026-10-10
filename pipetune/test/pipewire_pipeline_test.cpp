/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/dsp_pipeline.h"
#include "pipetune/pipewire_pipeline.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"
#include "startup_pipeline.h"

#include <yyjson.h>

#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

static bool pipeWireSessionIsAvailable() {
  const auto *runtimeDirectory = std::getenv("XDG_RUNTIME_DIR");
  if (runtimeDirectory == nullptr || runtimeDirectory[0] == '\0') {
    return false;
  }
  const auto *configuredRemote = std::getenv("PIPEWIRE_REMOTE");
  const auto remote =
      configuredRemote == nullptr || configuredRemote[0] == '\0'
          ? std::filesystem::path("pipewire-0")
          : std::filesystem::path(configuredRemote);
  const auto socket = remote.is_absolute()
                          ? remote
                          : std::filesystem::path(runtimeDirectory) / remote;
  return std::filesystem::exists(socket);
}

static void countReadyNotification(void *userData) {
  auto &count = *static_cast<int *>(userData);
  ++count;
}

static void reportReadyToParent(void *userData) {
  const auto descriptor = *static_cast<int *>(userData);
  constexpr auto marker = char{'R'};
  auto result = ssize_t{-1};
  do {
    result = write(descriptor, &marker, 1);
  } while (result < 0 && errno == EINTR);
}

static bool testMismatchedFixedDspRateIsRejected() {
  auto created = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64});
  if (!check(created.pipeline != nullptr, created.error)) {
    return false;
  }
  const auto result = pipetune::runPipeWirePipeline(
      std::move(created.pipeline),
      {.filterName = "pipetune_invalid_fixed_rate_test",
       .filterDescription = "PipeTune invalid fixed-rate test",
       .initialPresetPath = {},
       .initialConfigurationError = {},
       .controlSocketPath = {},
       .dspSampleRate = 48000,
       .ratePolicy =
           {.mode = pipetune::SampleRateMode::fixed,
            .fixedRate = 96000,
            .enforcement =
                pipetune::SampleRateEnforcement::force},
       .channelCount = 2,
       .maxFrames = 64,
       .ringCapacityFrames = 64,
       .readyCallback = nullptr,
       .readyUserData = nullptr},
      pipetune::PipeWireRunMode::untilReady);
  return check(!result.success &&
                   result.error ==
                       "fixed DSP sample rate must match the configured rate",
               "a fixed policy must reject a different initial DSP rate");
}

static bool testSixteenChannelPipeWireBounds() {
  auto accepted = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 16, .maxFrames = 64});
  if (!check(accepted.pipeline != nullptr, accepted.error)) {
    return false;
  }
  const auto acceptedResult = pipetune::runPipeWirePipeline(
      std::move(accepted.pipeline),
      {.filterName = "pipetune_sixteen_channel_validation_test",
       .filterDescription = "PipeTune 16-channel validation test",
       .initialPresetPath = {},
       .initialConfigurationError = {},
       .controlSocketPath = {},
       .dspSampleRate = 96000,
       .ratePolicy = pipetune::defaultSampleRatePolicy(),
       .channelCount = 16,
       .maxFrames = 64,
       .ringCapacityFrames = 64,
       .readyCallback = nullptr,
       .readyUserData = nullptr},
      pipetune::PipeWireRunMode::untilReady);
  if (!check(!acceptedResult.success &&
                 acceptedResult.error ==
                     "DSP pipeline format does not cover the requested PipeWire format",
             "16 channels must pass channel validation before format validation")) {
    return false;
  }

  auto rejected = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 16, .maxFrames = 64});
  if (!check(rejected.pipeline != nullptr, rejected.error)) {
    return false;
  }
  const auto rejectedResult = pipetune::runPipeWirePipeline(
      std::move(rejected.pipeline),
      {.filterName = "pipetune_channel_limit_validation_test",
       .filterDescription = "PipeTune channel-limit validation test",
       .initialPresetPath = {},
       .initialConfigurationError = {},
       .controlSocketPath = {},
       .dspSampleRate = 48000,
       .ratePolicy = pipetune::defaultSampleRatePolicy(),
       .channelCount = 17,
       .maxFrames = 64,
       .ringCapacityFrames = 64,
       .readyCallback = nullptr,
       .readyUserData = nullptr},
      pipetune::PipeWireRunMode::untilReady);
  return check(!rejectedResult.success &&
                   rejectedResult.error ==
                       "PipeWire channel count must be between one and sixteen",
               "PipeWire must reject more than 16 channels");
}

static bool responseHasLivePreset(std::string_view response,
                                  const std::filesystem::path &presetPath,
                                  std::size_t expectedWarningCount) {
  auto *document = yyjson_read(response.data(), response.size(), 0);
  if (document == nullptr) {
    return false;
  }
  auto *root = yyjson_doc_get_root(document);
  auto *preset = yyjson_is_obj(root) ? yyjson_obj_get(root, "preset") : nullptr;
  auto *warnings =
      yyjson_is_obj(root) ? yyjson_obj_get(root, "warnings") : nullptr;
  const auto matches =
      yyjson_is_str(preset) &&
      std::string_view(yyjson_get_str(preset), yyjson_get_len(preset)) ==
      presetPath.string() &&
      yyjson_get_uint(yyjson_obj_get(root, "activePluginCount")) == 1 &&
      yyjson_is_arr(warnings) &&
      yyjson_arr_size(warnings) == expectedWarningCount;
  yyjson_doc_free(document);
  return matches;
}

static bool testIndependentInputWidth(bool connect) {
  for (const auto width : {4U, 16U}) {
    auto created = pipetune::createBypassDspPipeline({
        .sampleRate = 48000, .maxChannels = width, .maxFrames = 8192});
    if (!check(created.pipeline != nullptr, created.error)) return false;
    auto notifications = 0;
    const auto result = pipetune::runPipeWirePipeline(std::move(created.pipeline), {
        .filterName = "pipetune_input_width_test",
        .filterDescription = "PipeTune stereo input test", .initialPresetPath = {},
        .initialConfigurationError = {}, .controlSocketPath = {},
        .dspSampleRate = connect ? 48000U : 96000U,
        .ratePolicy = pipetune::defaultSampleRatePolicy(), .channelCount = width,
        .maxFrames = 8192, .ringCapacityFrames = 16384,
        .readyCallback = countReadyNotification, .readyUserData = &notifications,
        .inputChannelCount = connect ? 2U : width + 1}, pipetune::PipeWireRunMode::untilReady);
    if (connect) {
      if (!check(result.success && result.processingErrors == 0 && notifications == 1,
                 "stereo input and wider DSP/output must negotiate independently: " + result.error)) return false;
    } else if (!check(!result.success && result.error == "PipeWire input channel count must not exceed the DSP width",
                      "invalid input width must fail before DSP format or PipeWire connection checks")) return false;
  }
  return true;
}

static std::optional<pipetune::ControlRuntimeStatus>
waitForInactiveGraph(const std::filesystem::path &socketPath) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(8);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto exchange = pipetune::exchangeControlMessage(
        socketPath, pipetune::makeStatusControlRequest());
    const auto parsed = pipetune::parseControlResponse(exchange.response);
    if (exchange.error.empty() && parsed.valid && parsed.success &&
        parsed.status.graphSampleRate == 0) {
      return parsed.status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return std::nullopt;
}

static std::optional<pipetune::ControlRuntimeStatus> waitForStatus(
    const std::filesystem::path &socketPath,
    const std::function<bool(const pipetune::ControlRuntimeStatus &)> &matches) {
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto exchange = pipetune::exchangeControlMessage(
        socketPath, pipetune::makeStatusControlRequest());
    const auto parsed = pipetune::parseControlResponse(exchange.response);
    if (exchange.error.empty() && parsed.valid && parsed.success &&
        matches(parsed.status)) {
      return parsed.status;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  return std::nullopt;
}

static bool writePresetContents(const std::filesystem::path &path,
                                std::string_view contents) {
  auto preset = std::ofstream(path, std::ios::binary | std::ios::trunc);
  preset.write(contents.data(), static_cast<std::streamsize>(contents.size()));
  preset.close();
  return preset.good();
}

static bool replacePresetContents(const std::filesystem::path &path,
                                  std::string_view contents) {
  auto temporary = path;
  temporary += ".replacement";
  if (!writePresetContents(temporary, contents)) {
    return false;
  }
  auto error = std::error_code{};
  std::filesystem::rename(temporary, path, error);
  return !error;
}

static bool testIrReload(const std::filesystem::path &socketPath,
                          const std::filesystem::path &presetPath) {
  const auto library = presetPath.parent_path() / "effetune" / "ir-library";
  std::filesystem::create_directories(library);
  auto bytes = std::vector<std::uint8_t>(44 + 512 * 4);
  const auto put = [&bytes](std::size_t offset, std::uint32_t value, unsigned width) {
    for (auto index = 0u; index < width; ++index) bytes[offset + index] = value >> (8 * index);
  };
  const auto text = [&bytes](std::size_t offset, std::string_view value) {
    std::copy(value.begin(), value.end(), bytes.begin() + offset);
  };
  text(0, "RIFF"); put(4, bytes.size() - 8, 4); text(8, "WAVEfmt "); put(16, 16, 4);
  put(20, 3, 2); put(22, 1, 2); put(24, 48000, 4); put(28, 192000, 4);
  put(32, 4, 2); put(34, 32, 2); text(36, "data"); put(40, 512 * 4, 4);
  put(44, std::bit_cast<std::uint32_t>(1.0F), 4);
  auto *digestText = g_compute_checksum_for_data(G_CHECKSUM_SHA256, bytes.data(), bytes.size());
  const auto digest = std::string(digestText);
  g_free(digestText);
  const auto id = digest.substr(0, 24);
  const auto source = library / (id + ".wav");
  {
    auto stream = std::ofstream(source, std::ios::binary);
    stream.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  }
  std::ofstream(library / "index.json") << "{\"version\":1,\"entries\":{\"" << id << "\":{\"irId\":\"" << id <<
      "\",\"composition\":\"single\",\"bytes\":" << bytes.size() << ",\"originals\":[{\"role\":\"single\",\"fileName\":\"impulse.wav\",\"storageName\":\"" <<
      id << ".wav\",\"sha256\":\"" << digest << "\",\"byteLength\":" << bytes.size() << "}]}}}";
  const auto path = presetPath.parent_path() / "ir.effetune_preset";
  const auto preset = "{\"pipeline\":[{\"name\":\"IR Reverb\",\"parameters\":{\"ir\":\"" + id + "\",\"cr\":\"full\",\"lt\":128}}]}";
  if (!writePresetContents(path, preset)) return false;
  const auto load = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(path));
  const auto active = pipetune::parseControlResponse(load.response);
  if (!check(load.error.empty() && active.success && active.status.activePluginCount == 1 &&
             active.status.dspLatencyFrames == 128, "registered IR did not activate on the live control path")) return false;
  const auto saved = library / (id + ".saved");
  std::filesystem::rename(source, saved);
  const auto missing = waitForStatus(socketPath, [](const auto &status) {
    return !status.configurationError.empty();
  });
  if (!check(missing && missing->activePreset == path.string() && missing->activePluginCount == 1 &&
      missing->dspLatencyFrames == 128 && missing->configurationRevision == active.status.configurationRevision,
      "missing IR source changed the active pipeline, latency, or configuration")) return false;
  const auto unavailableRate = pipetune::exchangeControlMessage(socketPath, pipetune::makeSetRateControlRequest(
      {.mode = pipetune::SampleRateMode::fixed, .fixedRate = active.status.dspSampleRate == 96000 ? 48000u : 96000u,
       .enforcement = pipetune::SampleRateEnforcement::force}));
  const auto unavailableBackend = pipetune::exchangeControlMessage(socketPath, pipetune::makeSetDspBackendControlRequest(
      active.status.effectiveDspBackend == pipetune::DspBackendKind::scalar ?
          pipetune::DspBackendKind::simd : pipetune::DspBackendKind::scalar));
  const auto retained = pipetune::parseControlResponse(pipetune::exchangeControlMessage(
      socketPath, pipetune::makeStatusControlRequest()).response);
  if (!check(unavailableRate.error.empty() && !pipetune::parseControlResponse(unavailableRate.response).success &&
      unavailableBackend.error.empty() && !pipetune::parseControlResponse(unavailableBackend.response).success &&
      retained.success && retained.status.activePreset == path.string() && retained.status.activePluginCount == 1 &&
      retained.status.dspLatencyFrames == 128 && retained.status.dspSampleRate == active.status.dspSampleRate &&
      retained.status.effectiveDspBackend == active.status.effectiveDspBackend &&
      retained.status.configuredRatePolicy == active.status.configuredRatePolicy && !retained.status.rateTransitioning,
      "failed rate or backend preparation must retain the old IR and effective configuration")) return false;
  std::filesystem::rename(saved, source);
  const auto restored = waitForStatus(socketPath, [&active](const auto &status) {
    return status.configurationError.empty() && status.configurationRevision > active.status.configurationRevision;
  });
  if (!check(restored && restored->dspLatencyFrames == 128, "restored original did not recover a failed IR reload")) return false;

  // A missing new dependency must be watched while the prior IR remains active.
  const auto index = library / "index.json";
  const auto savedIndex = library / "index.saved";
  std::filesystem::rename(index, savedIndex);
  const auto missingIndex = waitForStatus(socketPath, [](const auto &status) { return !status.configurationError.empty(); });
  if (!check(missingIndex && missingIndex->configurationRevision == restored->configurationRevision &&
      missingIndex->dspLatencyFrames == 128, "missing IR registry replaced the active pipeline")) return false;
  std::filesystem::rename(savedIndex, index);
  const auto recovered = check(waitForStatus(socketPath, [&restored](const auto &status) {
    return status.configurationError.empty() && status.configurationRevision > restored->configurationRevision &&
        status.dspLatencyFrames == 128;
  }).has_value(), "IR registry restoration did not recover automatically");
  const auto previous = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(presetPath));
  return recovered && check(previous.error.empty() && pipetune::parseControlResponse(previous.response).success,
                            "cannot restore the preceding preset after IR recovery");
}

static bool testSfzReload(const std::filesystem::path &socketPath,
                           const std::filesystem::path &precedingPreset) {
  const auto directory = precedingPreset.parent_path();
  const auto root = directory / "instrument";
  std::filesystem::create_directories(root);
  const auto registry = directory / "effetune" / "sfz-references.json";
  const auto selected = root / "main.sfz", included = root / "voice.sfz", sample = root / "a.wav";
  const auto path = directory / "sfz.effetune_preset";
  auto wave = std::vector<std::uint8_t>(44 + 512 * 4);
  const auto put = [&wave](std::size_t offset, std::uint32_t value, unsigned width) {
    for (auto index = 0u; index < width; ++index) wave[offset + index] = value >> (8 * index);
  };
  for (const auto &[offset, text] : std::array{std::pair{0u, "RIFF"}, std::pair{8u, "WAVE"},
      std::pair{12u, "fmt "}, std::pair{36u, "data"}}) std::copy_n(text, 4, wave.begin() + offset);
  put(4, wave.size() - 8, 4); put(16, 16, 4); put(20, 3, 2); put(22, 1, 2);
  put(24, 48000, 4); put(28, 192000, 4); put(32, 4, 2); put(34, 32, 2);
  put(40, 512 * 4, 4); put(44, std::bit_cast<std::uint32_t>(0.5F), 4);
  std::ofstream(sample, std::ios::binary).write(reinterpret_cast<const char *>(wave.data()), wave.size());
  std::ofstream(registry) << "[{\"id\":\"0123456789abcdef01234567\",\"name\":\"Live SFZ\",\"root\":\"" <<
      root.string() << "\",\"path\":\"" << selected.string() << "\"}]";
  const auto region = std::string("<region> sample=a.wav key=83 amp_veltrack=0 loop_mode=loop_continuous");
  if (!writePresetContents(selected, "#include \"voice.sfz\"") || !writePresetContents(included, region) ||
      !writePresetContents(path, R"({"pipeline":[{"name":"SFZ Note Player","parameters":{"sf":"0123456789abcdef01234567","mn":83,"mx":83}}]})")) return false;
  const auto loaded = pipetune::parseControlResponse(pipetune::exchangeControlMessage(
      socketPath, pipetune::makeLoadPresetControlRequest(path)).response);
  if (!check(loaded.success && loaded.status.activePluginCount == 1 && loaded.status.dspLatencyFrames > 0 &&
      loaded.status.sfzMaxSizeMiB == 128, "registered SFZ did not activate with the startup budget")) return false;
  auto revision = loaded.status.configurationRevision;
  const auto latency = loaded.status.dspLatencyFrames;
  for (const auto &dependency : {registry, selected, included, sample}) {
    const auto saved = std::filesystem::path(dependency.string() + ".saved");
    std::filesystem::rename(dependency, saved);
    const auto failed = waitForStatus(socketPath, [](const auto &status) { return !status.configurationError.empty(); });
    if (!check(failed && failed->configurationRevision == revision && failed->activePreset == path.string() &&
        failed->activePluginCount == 1 && failed->dspLatencyFrames == latency && failed->sfzMaxSizeMiB == 128,
        "a missing SFZ dependency replaced the prior pipeline or its effective settings")) return false;
    std::filesystem::rename(saved, dependency);
    const auto restored = waitForStatus(socketPath, [revision](const auto &status) {
      return status.configurationError.empty() && status.configurationRevision > revision;
    });
    if (!check(restored && restored->dspLatencyFrames == latency && restored->activePluginCount == 1,
        "restoring an SFZ dependency did not recover automatic reload")) return false;
    revision = restored->configurationRevision;
  }
  if (!replacePresetContents(included, region + " volume=-6\n<region> sample=later.wav key=84")) return false;
  const auto partial = waitForStatus(socketPath, [revision](const auto &status) {
    return status.configurationRevision > revision && status.activePluginCount == 1 &&
        !status.presetEntries.empty() && !status.presetEntries.front().diagnostics.empty();
  });
  if (!check(partial.has_value(), "partial SFZ must remain active with a missing-sample warning")) return false;
  std::ofstream(root / "later.wav", std::ios::binary).write(reinterpret_cast<const char *>(wave.data()), wave.size());
  const auto completed = waitForStatus(socketPath, [&partial](const auto &status) {
    return status.configurationRevision > partial->configurationRevision && status.configurationError.empty() &&
        !status.presetEntries.empty() && status.presetEntries.front().diagnostics.empty();
  });
  if (!check(completed && completed->dspLatencyFrames == latency,
      "a previously missing sample was not watched and restored automatically")) return false;
  const auto previous = pipetune::parseControlResponse(pipetune::exchangeControlMessage(
      socketPath, pipetune::makeLoadPresetControlRequest(precedingPreset)).response);
  return check(previous.success, "cannot restore the preceding preset after SFZ recovery");
}

static bool testCrosstalkReload(const std::filesystem::path &socketPath,
                                const std::filesystem::path &presetPath) {
  const auto root = presetPath.parent_path() / "effetune" / "measurement-backups";
  const auto preset = R"json({"pipeline":[
    {"name":"Volume","enabled":true,"parameters":{}},
    {"name":"Crosstalk Cancellation","enabled":true,"parameters":{
      "ll":"ear-l::ch=left","rl":"ear-l::ch=right",
      "lr":"ear-r::ch=left","rr":"ear-r::ch=right","tp":1024}}
  ]})json";
  if (!replacePresetContents(presetPath, preset)) return false;
  const auto omitted = waitForStatus(socketPath, [](const auto &status) {
    return status.activePluginCount == 1 && status.configurationError.find("measurement") != std::string::npos;
  });
  if (!check(omitted.has_value(), "missing Crosstalk measurements must be reported")) return false;
  std::filesystem::create_directories(root);
  for (const auto &id : {std::string("ear-l"), std::string("ear-r")}) {
    const auto left = id == "ear-l" ? "AACAPwAAAAA=" : "AAAAPwAAAAA=";
    const auto right = id == "ear-l" ? "AAAAPwAAAAA=" : "AACAPwAAAAA=";
    const auto json = "{\"id\":\"" + id + "\",\"outputChannels\":[\"left\",\"right\"],"
        "\"points\":[{\"pointId\":0,\"channels\":["
        "{\"channel\":\"left\",\"irId\":0,\"ir\":{\"stored\":true}},"
        "{\"channel\":\"right\",\"irId\":1,\"ir\":{\"stored\":true}}]}],"
        "\"impulseResponses\":["
        "{\"measurementId\":\"" + id + "\",\"pointId\":0,\"channel\":\"left\",\"data\":\"" + left + "\","
        "\"sampleRate\":48000,\"trimStartSamples\":0,\"onsetIndex\":0,\"outputTimeReference\":\"file\"},"
        "{\"measurementId\":\"" + id + "\",\"pointId\":1,\"channel\":\"right\",\"data\":\"" + right + "\","
        "\"sampleRate\":48000,\"trimStartSamples\":0,\"onsetIndex\":0,\"outputTimeReference\":\"file\"}]}";
    if (!replacePresetContents(root / (id + ".json"), json)) return false;
  }
  const auto active = waitForStatus(socketPath, [&omitted](const auto &status) {
    return status.activePluginCount == 2 && status.configurationError.empty() &&
        status.configurationRevision > omitted->configurationRevision;
  });
  if (!check(active.has_value(), "measurement creation must activate Crosstalk without GTK")) return false;
  const auto source = root / "ear-l.json";
  const auto backup = root / "ear-l.saved";
  std::filesystem::rename(source, backup);
  const auto removed = waitForStatus(socketPath, [&active](const auto &status) {
    return status.activePluginCount == 1 && !status.configurationError.empty() &&
        status.configurationRevision > active->configurationRevision;
  });
  if (!check(removed.has_value(), "measurement deletion must omit only Crosstalk")) return false;
  std::filesystem::rename(backup, source);
  const auto recovered = waitForStatus(socketPath, [&removed](const auto &status) {
    return status.activePluginCount == 2 && status.configurationError.empty() &&
        status.configurationRevision > removed->configurationRevision;
  });
  if (!check(recovered.has_value(), "measurement restoration must recover Crosstalk")) return false;
  if (!replacePresetContents(presetPath, R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{}}]})json")) return false;
  return check(waitForStatus(socketPath, [](const auto &status) {
    return status.activePluginCount == 1 && status.configurationError.empty();
  }).has_value(), "preset switching must remove measurement dependencies");
}

static bool testBassManagementLiveRejection(const std::filesystem::path &socketPath,
                                           const std::filesystem::path &presetPath) {
  const auto validPath = presetPath.parent_path() / "valid-bass.effetune_preset";
  if (!writePresetContents(validPath,
        R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"su":2,"ro":[1,3],"rt":[2]}}]})")) return false;
  const auto loaded = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(validPath));
  auto active = pipetune::parseControlResponse(loaded.response);
  if (!check(loaded.error.empty() && active.success && responseHasLivePreset(loaded.response, validPath, 0),
             "Bass Management live load must activate without warnings")) return false;
  for (const auto phase : {"Linear", "IIR", "Linear"}) {
    const auto linear = std::string_view(phase) == "Linear";
    const auto latency = linear ? 16512u : 0u;
    const auto content = std::string(R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"ph":")") +
        phase + R"(","tp":"32768","su":2,"ro":[1,3],"rt":[2]}}]})";
    if (!replacePresetContents(validPath, content)) return false;
    const auto reloaded = waitForStatus(socketPath, [&](const auto &status) {
      return status.configurationRevision > active.status.configurationRevision &&
             status.dspLatencyFrames == latency && status.configurationError.empty();
    });
    if (!check(reloaded.has_value(), "Bass Management IIR/Linear file reload must publish its new latency")) return false;
    const auto rate = linear ? 96000u : 48000u;
    const auto changed = pipetune::exchangeControlMessage(socketPath, pipetune::makeSetRateControlRequest(
        {.mode = pipetune::SampleRateMode::fixed, .fixedRate = rate}));
    active = pipetune::parseControlResponse(changed.response);
    if (!check(changed.error.empty() && active.success && active.status.dspLatencyFrames == latency &&
                   active.status.dspSampleRate == rate &&
                   responseHasLivePreset(changed.response, validPath, 0),
               "Bass Management rate rebuild must retain its FIR latency and preset")) return false;
  }
  const auto invalidPath = presetPath.parent_path() / "invalid-bass.effetune_preset";
  if (!writePresetContents(invalidPath,
        R"({"pipeline":[{"name":"Bass Management","channel":"All","parameters":{"su":8}}]})")) return false;
  const auto rejected = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(invalidPath));
  const auto response = pipetune::inspectControlResponse(rejected.response);
  const auto unchanged = pipetune::exchangeControlMessage(socketPath, pipetune::makeStatusControlRequest());
  const auto parsed = pipetune::parseControlResponse(unchanged.response);
  return check(rejected.error.empty() && !response.success &&
                   rejected.response.find("Bass Management") != std::string::npos &&
                   unchanged.error.empty() && parsed.success &&
                   parsed.status.configurationRevision == active.status.configurationRevision &&
                   responseHasLivePreset(unchanged.response, validPath, 0),
               "invalid Bass Management live load must retain the previous preset and DSP count");
}

static bool testMeasurementLiveChanges(const std::filesystem::path &socketPath,
                                       const std::filesystem::path &presetPath) {
  const auto path = presetPath.parent_path() / "measurement-live.effetune_preset";
  const auto content = R"({"pipeline":[
    {"name":"Tonal Balance EQ","parameters":{"at":100}},
    {"name":"Rhythm Analyzer","parameters":{"ck":true}},
    {"name":"Analog Meter"},
    {"name":"Rhythm Analyzer","enabled":false}
  ]})";
  if (!writePresetContents(path, content)) return false;
  using State = pipetune::PresetEntryState;
  auto entries = std::vector<pipetune::PresetEntry>{
      {"Tonal Balance EQ", State::enabled}, {"Rhythm Analyzer", State::enabled},
      {"Analog Meter", State::ignored}, {"Rhythm Analyzer", State::off}};
  const auto loaded = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(path));
  auto active = pipetune::parseControlResponse(loaded.response);
  if (!check(loaded.error.empty() && active.success && active.status.presetEntries == entries &&
                 active.status.activePluginCount == 2 && active.status.dspLatencyFrames == 0 &&
                 active.warnings.empty(), "measurement preset live load must publish all entry states")) return false;
  for (const auto rate : {48000u, 96000u}) {
    const auto previousRevision = active.status.configurationRevision;
    const auto changed = pipetune::exchangeControlMessage(socketPath,
        pipetune::makeSetRateControlRequest({.mode = pipetune::SampleRateMode::fixed, .fixedRate = rate}));
    active = pipetune::parseControlResponse(changed.response);
    if (!check(changed.error.empty() && active.success && active.status.dspSampleRate == rate &&
                   active.status.presetEntries == entries && active.status.activePluginCount == 2 &&
                   active.status.configurationRevision >= previousRevision,
               "rate rebuild must retain new DSP entry states")) return false;
    const auto backend = rate == 48000u ? pipetune::DspBackendKind::scalar : pipetune::DspBackendKind::simd;
    const auto switched = pipetune::exchangeControlMessage(socketPath, pipetune::makeSetDspBackendControlRequest(backend));
    active = pipetune::parseControlResponse(switched.response);
    if (!check(switched.error.empty() && active.success && active.status.effectiveDspBackend == backend &&
                   active.status.presetEntries == entries && active.status.activePluginCount == 2,
               "backend rebuild must retain new DSP entry states")) return false;
  }
  if (!replacePresetContents(path, R"({"pipeline":[
    {"name":"Tonal Balance EQ","parameters":{"at":100}},
    {"name":"Rhythm Analyzer","parameters":{"ck":false}},
    {"name":"Analog Meter"},
    {"name":"Rhythm Analyzer","enabled":true,"parameters":{"ck":false}}
  ]})")) return false;
  entries.back().state = State::enabled;
  const auto updated = waitForStatus(socketPath, [&](const auto &status) {
    return status.configurationRevision > active.status.configurationRevision &&
        status.presetEntries == entries && status.activePluginCount == 3 && status.configurationError.empty();
  });
  if (!check(updated.has_value(), "file reload must activate click-off Rhythm as a normal DSP")) return false;
  if (!replacePresetContents(path, R"({"pipeline":[)")) return false;
  const auto rejected = waitForStatus(socketPath, [&](const auto &status) {
    return !status.configurationError.empty() && status.configurationRevision == updated->configurationRevision;
  });
  return check(rejected.has_value() && rejected->presetEntries == entries &&
                   rejected->activePluginCount == 3 && rejected->activePreset == path.string(),
               "failed reload must retain the measured pipeline and its reported configuration");
}

static bool testOversamplingReload(const std::filesystem::path &socketPath,
                                   const std::filesystem::path &presetPath) {
  const auto path = presetPath.parent_path() / "oversampling.effetune_preset";
  for (const auto name : {"Saturation", "Dynamic Saturation", "Exciter", "Hard Clipping",
                          "Harmonic Distortion", "Multiband Saturation"}) {
    const auto prefix = "{\"pipeline\":[{\"name\":\"" + std::string(name) +
        "\",\"channel\":\"All\",\"parameters\":{\"os\":";
    if (!writePresetContents(path, prefix + "1}}]}")) return false;
    const auto loaded = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(path));
    auto active = pipetune::parseControlResponse(loaded.response);
    if (!check(loaded.error.empty() && active.success && responseHasLivePreset(loaded.response, path, 0),
               "OS preset must activate without warnings")) return false;
    for (const auto factor : {2u, 8u, 3u, 1u}) {
      if (!replacePresetContents(path, prefix + std::to_string(factor) + "}}]}")) return false;
      const auto updated = waitForStatus(socketPath, [&](const auto &status) {
        return status.configurationRevision > active.status.configurationRevision &&
            status.activePluginCount == 1u && status.configurationError.empty() &&
            status.dspLatencyFrames == (factor == 2u || factor == 8u ? 64u : 0u);
      });
      if (!check(updated.has_value(), std::string(name) + " file reload must publish OS latency")) return false;
      active.status = *updated;
    }
  }
  const auto mp3 = std::string(R"({"pipeline":[{"name":"MP3 Codec Simulator","channel":"All","parameters":{"cr":")");
  if (!writePresetContents(path, mp3 + "44.1 kHz (MPEG-1)\"}}]}")) return false;
  const auto loaded = pipetune::exchangeControlMessage(socketPath, pipetune::makeLoadPresetControlRequest(path));
  auto active = pipetune::parseControlResponse(loaded.response);
  if (!check(loaded.error.empty() && active.success && responseHasLivePreset(loaded.response, path, 0),
             "MP3 preset must activate without warnings")) return false;
  for (const auto profile : {"22.05 kHz (MPEG-2)", "44.1 kHz (MPEG-1)"}) {
    if (!replacePresetContents(path, mp3 + profile + "\"}}]}")) return false;
    const auto updated = waitForStatus(socketPath, [&](const auto &status) {
      return status.configurationRevision > active.status.configurationRevision &&
          status.activePluginCount == 1u && status.configurationError.empty() &&
          status.dspLatencyFrames == active.status.dspLatencyFrames;
    });
    if (!check(updated.has_value(), "MP3 profile reload must retain its fixed per-rate latency")) return false;
    active.status = *updated;
  }
  return true;
}

static int runSignalChild(std::string_view processId, const std::filesystem::path &initialPresetPath,
    const std::filesystem::path &socketPath, std::uint32_t initialDspSampleRate,
    pipetune::SampleRatePolicy initialRatePolicy, int readyDescriptor) {
  const auto signals = pipetune::blockPipeWireTerminationSignals();
  if (!check(signals.empty(), signals)) return 1;
  auto loaded = pipetune::loadDspPipeline(initialPresetPath, {static_cast<float>(initialDspSampleRate), 2, 8192});
  if (!loaded.pipeline) return 1;
    const auto result = pipetune::runPipeWirePipeline(
        std::move(loaded.pipeline),
        {.filterName = "pipetune_signal_test_" + std::string(processId),
         .filterDescription = "PipeTune signal integration test",
         .initialPresetPath = initialPresetPath,
         .initialConfigurationError = {},
         .controlSocketPath = socketPath,
         .dspSampleRate = initialDspSampleRate,
         .ratePolicy = initialRatePolicy,
         .channelCount = 2,
         .maxFrames = 8192,
         .ringCapacityFrames = 16384,
         .readyCallback = reportReadyToParent,
         .readyUserData = &readyDescriptor,
         .sfzMaxSizeMiB = 128},
        pipetune::PipeWireRunMode::untilInterrupted);
    if (!result.success) {
      std::cerr << result.error << '\n';
    }
    close(readyDescriptor);
  return result.success ? 0 : 1;
}

static bool testOrderlySignalShutdown(
    std::unique_ptr<pipetune::DspPipeline> pipeline,
    std::string_view processId,
    const std::filesystem::path &initialPresetPath,
    const std::filesystem::path &replacementPresetPath,
    const std::filesystem::path &socketPath,
    std::uint32_t initialDspSampleRate,
    pipetune::SampleRatePolicy initialRatePolicy) {
  auto descriptors = std::array<int, 2>{-1, -1};
  if (!check(pipe(descriptors.data()) == 0,
             "cannot create readiness pipe for signal test")) {
    return false;
  }

  const auto childId = std::string(processId);
  const auto childPreset = initialPresetPath.string();
  const auto childSocket = socketPath.string();
  const auto childRate = std::to_string(initialDspSampleRate);
  const auto childEnforcement = std::to_string(static_cast<int>(initialRatePolicy.enforcement));
  const auto childDescriptor = std::to_string(descriptors[1]);
  pipeline.reset();
  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    return check(false, "cannot fork PipeWire signal test");
  }
  if (child == 0) {
    // The parent has initialized GLib's I/O threads. Start a fresh image before
    // using GIO or PipeWire, just as the real daemon launcher does.
    close(descriptors[0]);
    execl("/proc/self/exe", "pipetune_pipewire_pipeline_tests", "--signal-child",
          childId.c_str(), childPreset.c_str(), childSocket.c_str(), childRate.c_str(),
          childEnforcement.c_str(), childDescriptor.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }

  close(descriptors[1]);
  auto marker = char{0};
  auto readResult = ssize_t{-1};
  do {
    readResult = read(descriptors[0], &marker, 1);
  } while (readResult < 0 && errno == EINTR);
  close(descriptors[0]);
  if (readResult != 1 || marker != 'R') {
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return check(false, "child pipeline did not report readiness");
  }

  const auto status = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeStatusControlRequest());
  const auto parsedStatus =
      pipetune::parseControlResponse(status.response);
  if (!check(status.error.empty(), status.error) ||
      !check(pipetune::inspectControlResponse(status.response).success,
             "initial status request failed") ||
      !check(parsedStatus.valid, parsedStatus.error) ||
      !check(parsedStatus.status.inputSampleFormat == "F32P" &&
                 parsedStatus.status.inputSampleRate ==
                     initialDspSampleRate &&
                 parsedStatus.status.dspSampleRate ==
                     initialDspSampleRate &&
                 parsedStatus.status.inputChannelCount == 2 &&
                 parsedStatus.status.configuredRatePolicy ==
                     initialRatePolicy &&
                 !parsedStatus.status.rateTransitioning,
             "fixed-rate startup does not report its configured DSP format")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto rateChange = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeSetRateControlRequest(
          {.mode = pipetune::SampleRateMode::fixed,
           .fixedRate = 96000,
           .enforcement = pipetune::SampleRateEnforcement::force}));
  const auto parsedRate =
      pipetune::parseControlResponse(rateChange.response);
  if (!check(rateChange.error.empty(), rateChange.error) ||
      !check(parsedRate.valid, parsedRate.error) ||
      !check(parsedRate.success,
             "live rate request failed") ||
      !check(parsedRate.status.configuredRatePolicy ==
                     pipetune::SampleRatePolicy{
                         .mode = pipetune::SampleRateMode::fixed,
                         .fixedRate = 96000,
                         .enforcement =
                             pipetune::SampleRateEnforcement::force} &&
                 parsedRate.status.dspSampleRate == 96000 &&
                 parsedRate.status.inputSampleRate == 96000 &&
                 !parsedRate.status.rateTransitioning,
             "live rate response does not report completed renegotiation")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto backendChange = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeSetDspBackendControlRequest(
          pipetune::DspBackendKind::simd));
  const auto parsedBackend =
      pipetune::parseControlResponse(backendChange.response);
  if (!check(backendChange.error.empty(), backendChange.error) ||
      !check(parsedBackend.valid, parsedBackend.error) ||
      !check(parsedBackend.success,
             "live DSP backend request failed") ||
      !check(parsedBackend.status.configuredDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 parsedBackend.status.effectiveDspBackend ==
                     pipetune::DspBackendKind::simd &&
                 !parsedBackend.status.dspBackendFallback &&
                 parsedBackend.status.availableDspBackends[0].available &&
                 parsedBackend.status.availableDspBackends[1].available,
             "live DSP backend response does not report effective SIMD")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto load = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeLoadPresetControlRequest(replacementPresetPath));
  const auto parsedLoad = pipetune::parseControlResponse(load.response);
  if (!check(load.error.empty(), load.error) ||
      !check(parsedLoad.valid, parsedLoad.error) ||
      !check(parsedLoad.success, "live preset request failed") ||
      !check(parsedLoad.status.presetEntries ==
                 std::vector<pipetune::PresetEntry>{
                     {"Future DSP", pipetune::PresetEntryState::ignored,
                      parsedLoad.warnings.empty() ? std::vector<std::string>{} :
                          std::vector<std::string>{parsedLoad.warnings.front().reason}},
                     {"Volume", pipetune::PresetEntryState::enabled}},
             "live status must report the loaded and ignored entries") ||
      !check(responseHasLivePreset(load.response, replacementPresetPath, 1),
             "live preset response does not report the active replacement")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  if (!check(replacePresetContents(
                 replacementPresetPath,
                 R"json({"pipeline":[
      {"name":"Volume","enabled":true,"parameters":{"vl":-3},"channel":"A"},
      {"name":"Volume","enabled":true,"parameters":{"vl":-4},"channel":"A"}
    ]})json"),
             "cannot atomically update the active preset")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }
  const auto automaticallyReloaded = waitForStatus(
      socketPath, [&parsedLoad](const auto &status) {
        return status.activePluginCount == 2 &&
               status.configurationError.empty() &&
               status.configurationRevision >
                   parsedLoad.status.configurationRevision;
      });
  if (!check(automaticallyReloaded.has_value(),
             "active preset replacement was not loaded automatically") ||
      !check(automaticallyReloaded->presetEntries ==
                 std::vector<pipetune::PresetEntry>{
                     {"Volume", pipetune::PresetEntryState::enabled},
                     {"Volume", pipetune::PresetEntryState::enabled}},
             "automatic reload must replace the reported configuration")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  if (!check(writePresetContents(replacementPresetPath,
                                 R"json({"pipeline":[)json"),
             "cannot write a malformed active preset")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }
  const auto rejectedReload = waitForStatus(
      socketPath, [&automaticallyReloaded](const auto &status) {
        return !status.configurationError.empty() &&
               status.configurationRevision ==
                   automaticallyReloaded->configurationRevision;
      });
  if (!check(rejectedReload.has_value(),
             "malformed automatic reload did not report an error") ||
      !check(rejectedReload->activePluginCount == 2 &&
                 rejectedReload->presetEntries == automaticallyReloaded->presetEntries &&
                 rejectedReload->activePreset ==
                     replacementPresetPath.string(),
             "malformed automatic reload changed the active preset")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  if (!check(replacePresetContents(
                 replacementPresetPath,
                 R"json({"pipeline":[
      {"name":"Volume","enabled":true,"parameters":{"vl":-9},"channel":"A"}
    ]})json"),
             "cannot repair the active preset")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }
  const auto recoveredReload = waitForStatus(
      socketPath, [&rejectedReload](const auto &status) {
        return status.activePluginCount == 1 &&
               status.configurationError.empty() &&
               status.configurationRevision >
                   rejectedReload->configurationRevision;
      });
  if (!check(recoveredReload.has_value(),
             "automatic preset reload did not recover after repair")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto rejected = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeLoadPresetControlRequest(
                      replacementPresetPath.parent_path() /
                      "missing.effetune_preset"));
  const auto statusAfterFailure = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeStatusControlRequest());
  if (!check(rejected.error.empty(), rejected.error) ||
      !check(!pipetune::inspectControlResponse(rejected.response).success,
             "missing live preset must be rejected") ||
      !check(statusAfterFailure.error.empty(), statusAfterFailure.error) ||
      !check(responseHasLivePreset(statusAfterFailure.response,
                                   replacementPresetPath, 0),
             "failed loading must leave the previous preset active")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  if (!testIrReload(socketPath, replacementPresetPath) ||
      !testSfzReload(socketPath, replacementPresetPath) ||
      !testCrosstalkReload(socketPath, replacementPresetPath) ||
      !testBassManagementLiveRejection(socketPath, replacementPresetPath) ||
      !testMeasurementLiveChanges(socketPath, replacementPresetPath) ||
      !testOversamplingReload(socketPath, replacementPresetPath)) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  const auto inactive = waitForInactiveGraph(socketPath);
  if (!check(inactive.has_value(),
             "PipeWire filter graph did not become idle") ||
      !check(!inactive->rateTransitioning,
             "an idle graph must not remain in a sample-rate transition")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }
  const auto backendChangeWhileIdle = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeSetDspBackendControlRequest(
                      pipetune::DspBackendKind::scalar));
  const auto parsedIdleBackend =
      pipetune::parseControlResponse(backendChangeWhileIdle.response);
  if (!check(backendChangeWhileIdle.error.empty(),
             backendChangeWhileIdle.error) ||
      !check(parsedIdleBackend.valid, parsedIdleBackend.error) ||
      !check(parsedIdleBackend.success,
             "an idle graph must still accept DSP setting changes")) {
    kill(child, SIGTERM);
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return false;
  }

  if (kill(child, SIGTERM) != 0) {
    auto childStatus = 0;
    waitpid(child, &childStatus, 0);
    return check(false, "cannot signal child PipeWire pipeline");
  }

  auto childStatus = 0;
  auto waitResult = pid_t{-1};
  do {
    waitResult = waitpid(child, &childStatus, 0);
  } while (waitResult < 0 && errno == EINTR);
  if (!check(waitResult == child && WIFEXITED(childStatus) &&
                 WEXITSTATUS(childStatus) == 0,
             "SIGTERM must stop the PipeWire pipeline orderly")) {
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  if (argc == 8 && std::string_view(argv[1]) == "--signal-child") {
    const auto rate = static_cast<std::uint32_t>(std::stoul(argv[5]));
    return runSignalChild(argv[2], argv[3], argv[4], rate,
        {.mode = pipetune::SampleRateMode::fixed, .fixedRate = rate,
         .enforcement = static_cast<pipetune::SampleRateEnforcement>(std::stoi(argv[6]))}, std::stoi(argv[7]));
  }
  if (!testMismatchedFixedDspRateIsRejected() ||
      !testSixteenChannelPipeWireBounds() || !testIndependentInputWidth(false)) {
    return 1;
  }
  if (!pipeWireSessionIsAvailable()) {
    std::cout << "PipeWire session socket is unavailable; skipping integration test\n";
    return 77;
  }
  if (!testIndependentInputWidth(true)) return 1;

  const auto processId = std::to_string(static_cast<long long>(getpid()));
  const auto directory =
      std::filesystem::temp_directory_path() / ("pipetune-pipewire-test-" + processId);
  std::filesystem::create_directories(directory);
  setenv("XDG_CONFIG_HOME", directory.c_str(), 1);
  setenv("XDG_CACHE_HOME", (directory / "cache").c_str(), 1);
  const auto presetPath = directory / "empty.effetune_preset";
  {
    auto preset = std::ofstream(presetPath, std::ios::binary);
    preset << R"json({"name":"PipeWire test","pipeline":[],"timestamp":1})json";
  }
  const auto replacementPresetPath =
      directory / "replacement.effetune_preset";
  {
    auto preset = std::ofstream(replacementPresetPath, std::ios::binary);
    preset << R"json({"pipeline":[
      {"name":"Future DSP","enabled":true,"parameters":{}},
      {"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}
    ]})json";
  }
  const auto socketPath = directory / "control.sock";
  const auto configPath = directory / "environment";
  const auto startupRatePolicy = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 192000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  const auto savedConfig = pipetune::saveStartupConfig(
      configPath,
      {.presetFound = true,
       .presetPath = presetPath,
       .ratePolicy = startupRatePolicy,
       .dspBackend = pipetune::DspBackendKind::scalar,
       .dspSimdVariant = pipetune::DspSimdVariant::automatic});
  if (!check(savedConfig.empty(), savedConfig)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto signalPipeline = pipetune::prepareStartupPipeline(
      configPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 8192});
  if (!check(signalPipeline.pipeline != nullptr, signalPipeline.error) ||
      !check(signalPipeline.pipeline->sampleRate() == 192000.0F,
             "fixed-rate startup did not prepare the configured DSP rate")) {
    std::filesystem::remove_all(directory);
    return 1;
  }
  const auto initialDspSampleRate = static_cast<std::uint32_t>(
      signalPipeline.pipeline->sampleRate());

  if (!testOrderlySignalShutdown(
          std::move(signalPipeline.pipeline), processId, presetPath,
          replacementPresetPath, socketPath, initialDspSampleRate,
          signalPipeline.ratePolicy)) {
    std::filesystem::remove_all(directory);
    return 1;
  }

  auto readyPipeline = pipetune::loadDspPipeline(
      presetPath,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 8192});
  if (!check(readyPipeline.pipeline != nullptr, readyPipeline.error)) {
    std::filesystem::remove_all(directory);
    return 1;
  }
  auto readyNotifications = 0;
  const auto result = pipetune::runPipeWirePipeline(
      std::move(readyPipeline.pipeline),
      {.filterName = "pipetune_test_" + processId,
       .filterDescription = "PipeTune integration test",
       .initialPresetPath = presetPath,
       .initialConfigurationError = {},
       .controlSocketPath = {},
       .dspSampleRate = 48000,
       .ratePolicy = pipetune::defaultSampleRatePolicy(),
       .channelCount = 2,
       .maxFrames = 8192,
       .ringCapacityFrames = 16384,
       .readyCallback = countReadyNotification,
       .readyUserData = &readyNotifications},
      pipetune::PipeWireRunMode::untilReady);

  std::filesystem::remove_all(directory);
  return check(result.success, result.error) &&
                 check(readyNotifications == 1,
                       "PipeWire readiness must be reported exactly once") &&
                 check(result.processingErrors == 0,
                       "readiness must not report processing errors")
             ? 0
             : 1;
}
