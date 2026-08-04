#include "pipetune/dsp_pipeline.h"
#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/pipewire_pipeline.h"

#include <yyjson.h>

#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <csignal>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

struct GraphNode {
  std::string name;
  std::unordered_map<std::string, std::string> properties;
};

struct ReadyInspection {
  bool inspected = false;
  std::string error;
  std::vector<GraphNode> graph;
  std::filesystem::path controlSocketPath;
  std::string controlResponse;
  std::string controlError;
};

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

static std::optional<std::string> dumpPipeWireGraph() {
  auto *stream = popen("pw-dump", "r");
  if (stream == nullptr) {
    return std::nullopt;
  }
  auto output = std::string{};
  auto buffer = std::array<char, 4096>{};
  while (true) {
    const auto count = fread(buffer.data(), 1, buffer.size(), stream);
    output.append(buffer.data(), count);
    if (count < buffer.size()) {
      break;
    }
  }
  if (pclose(stream) != 0) {
    return std::nullopt;
  }
  return output;
}

static std::string jsonScalar(yyjson_val *value) {
  if (yyjson_is_str(value)) {
    return std::string(yyjson_get_str(value), yyjson_get_len(value));
  }
  if (yyjson_is_uint(value)) {
    return std::to_string(yyjson_get_uint(value));
  }
  if (yyjson_is_sint(value)) {
    return std::to_string(yyjson_get_sint(value));
  }
  if (yyjson_is_real(value)) {
    return std::to_string(yyjson_get_real(value));
  }
  if (yyjson_is_bool(value)) {
    return yyjson_get_bool(value) ? "true" : "false";
  }
  return {};
}

static std::vector<GraphNode> parseGraph(std::string_view json,
                                         std::string &error) {
  auto *document = yyjson_read(json.data(), json.size(), 0);
  if (document == nullptr) {
    error = "cannot parse pw-dump output";
    return {};
  }
  auto nodes = std::vector<GraphNode>{};
  auto *root = yyjson_doc_get_root(document);
  std::size_t index = 0;
  std::size_t maximum = 0;
  yyjson_val *object = nullptr;
  yyjson_arr_foreach(root, index, maximum, object) {
    auto *type = yyjson_obj_get(object, "type");
    if (!yyjson_is_str(type) ||
        std::string_view(yyjson_get_str(type), yyjson_get_len(type)) !=
            "PipeWire:Interface:Node") {
      continue;
    }
    auto *info = yyjson_obj_get(object, "info");
    auto *properties =
        yyjson_is_obj(info) ? yyjson_obj_get(info, "props") : nullptr;
    if (!yyjson_is_obj(properties)) {
      continue;
    }
    auto node = GraphNode{};
    std::size_t propertyIndex = 0;
    std::size_t propertyMaximum = 0;
    yyjson_val *key = nullptr;
    yyjson_val *value = nullptr;
    yyjson_obj_foreach(properties, propertyIndex, propertyMaximum, key, value) {
      if (!yyjson_is_str(key)) {
        continue;
      }
      node.properties.emplace(
          std::string(yyjson_get_str(key), yyjson_get_len(key)),
          jsonScalar(value));
    }
    const auto name = node.properties.find("node.name");
    if (name != node.properties.end()) {
      node.name = name->second;
    }
    nodes.push_back(std::move(node));
  }
  yyjson_doc_free(document);
  return nodes;
}

static void inspectReadyGraph(void *userData) {
  auto &inspection = *static_cast<ReadyInspection *>(userData);
  inspection.inspected = true;
  const auto dump = dumpPipeWireGraph();
  if (!dump.has_value()) {
    inspection.error = "cannot run pw-dump while filters are ready";
    return;
  }
  inspection.graph = parseGraph(*dump, inspection.error);
  const auto exchange = pipetune::exchangeControlMessage(
      inspection.controlSocketPath, pipetune::makeStatusControlRequest());
  inspection.controlResponse = exchange.response;
  inspection.controlError = exchange.error;
}

static bool propertyIs(const GraphNode &node, std::string_view key,
                       std::string_view expected) {
  const auto found = node.properties.find(std::string(key));
  return found != node.properties.end() && found->second == expected;
}

static bool propertyNumberIs(const GraphNode &node, std::string_view key,
                             double expected) {
  const auto found = node.properties.find(std::string(key));
  if (found == node.properties.end()) {
    return false;
  }
  char *end = nullptr;
  const auto parsed = std::strtod(found->second.c_str(), &end);
  return end == found->second.c_str() + found->second.size() &&
         std::abs(parsed - expected) < 0.000001;
}

static std::unordered_map<std::string, GraphNode>
physicalOutputs(const std::vector<GraphNode> &graph) {
  auto outputs = std::unordered_map<std::string, GraphNode>{};
  for (const auto &node : graph) {
    const auto api = node.properties.find("device.api");
    const auto channels = node.properties.find("audio.channels");
    const auto positions = node.properties.find("audio.position");
    if (!propertyIs(node, "media.class", "Audio/Sink") ||
        propertyIs(node, "node.virtual", "true") ||
        node.properties.contains("node.link-group") ||
        propertyIs(node, "node.network", "true") ||
        !node.properties.contains("device.id") ||
        api == node.properties.end() ||
        (api->second != "alsa" && api->second != "bluez5") ||
        channels == node.properties.end() || positions == node.properties.end()) {
      continue;
    }
    const auto count = std::strtoul(channels->second.c_str(), nullptr, 10);
    if (count >= 1 && count <= 8) {
      outputs.emplace(node.name, node);
    }
  }
  return outputs;
}

static const GraphNode *findFilterForTarget(
    const std::vector<GraphNode> &graph, std::string_view target,
    bool playback) {
  for (const auto &node : graph) {
    if (propertyIs(node, "pipetune.target.node", target) &&
        propertyIs(node,
                   playback ? "pipetune.filter.stream" : "pipetune.filter",
                   "true")) {
      return &node;
    }
  }
  return nullptr;
}

static bool validateFilterPairs(const ReadyInspection &inspection) {
  if (!check(inspection.inspected, "service must invoke its ready callback") ||
      !check(inspection.error.empty(), inspection.error)) {
    return false;
  }
  const auto physical = physicalOutputs(inspection.graph);
  if (physical.empty()) {
    std::cout << "No eligible physical PipeWire outputs; skipping filter "
                 "service integration test\n";
    return true;
  }
  for (const auto &[targetName, target] : physical) {
    const auto *main =
        findFilterForTarget(inspection.graph, targetName, false);
    const auto *playback =
        findFilterForTarget(inspection.graph, targetName, true);
    if (main == nullptr || playback == nullptr) {
      std::cerr << "missing filter pair for target: " << targetName << '\n';
      for (const auto &candidate : inspection.graph) {
        const auto filterTarget =
            candidate.properties.find("pipetune.target.node");
        if (filterTarget != candidate.properties.end()) {
          std::cerr << "  PipeTune node " << candidate.name
                    << " targets " << filterTarget->second << '\n';
        }
      }
    }
    if (!check(main != nullptr,
               "each physical output must have one PipeTune main filter") ||
        !check(playback != nullptr,
               "each physical output must have one PipeTune playback stream") ||
        !check(propertyIs(*main, "media.class", "Audio/Sink") &&
                   propertyIs(*main, "filter.smart", "true") &&
                   propertyIs(*main, "filter.smart.disabled", "true") &&
                   propertyIs(*main, "node.virtual", "true") &&
                   propertyIs(*main, "state.restore-props", "false"),
               "main filter must be hidden-policy-ready and state-neutral") ||
        !check(propertyIs(*playback, "media.class", "Stream/Output/Audio") &&
                   propertyIs(*playback, "target.object", targetName) &&
                   propertyIs(*playback, "node.passive", "true") &&
                   propertyIs(*playback, "state.restore-props", "false"),
               "playback stream must passively target its physical output") ||
        !check(main->properties.at("node.link-group") ==
                   playback->properties.at("node.link-group"),
               "each filter pair must share one link group") ||
        !check(main->properties.at("audio.channels") ==
                   target.properties.at("audio.channels") &&
                   main->properties.at("audio.position") ==
                       target.properties.at("audio.position"),
               "filter must preserve the physical output layout exactly") ||
        !check(propertyNumberIs(*main, "channelmix.min-volume", 1.0) &&
                   propertyNumberIs(*main, "channelmix.max-volume", 1.0) &&
                   propertyNumberIs(*playback, "channelmix.min-volume", 1.0) &&
                   propertyNumberIs(*playback, "channelmix.max-volume", 1.0),
               "both internal nodes must remain at unity gain")) {
      return false;
    }
  }
  return true;
}

static bool validateOutputStatuses(
    const ReadyInspection &inspection,
    const pipetune::PipeWireFilterServiceResult &result) {
  const auto physical = physicalOutputs(inspection.graph);
  if (physical.empty()) {
    return true;
  }
  if (!check(result.outputs.size() == physical.size(),
             "service status must contain only physical output sinks")) {
    return false;
  }
  auto seen = std::unordered_map<std::string, bool>{};
  for (const auto &output : result.outputs) {
    if (!check(physical.contains(output.targetNodeName),
               "service status must not include its own filter nodes") ||
        !check(!seen.contains(output.targetNodeName),
               "each physical output must have exactly one status")) {
      return false;
    }
    seen.emplace(output.targetNodeName, true);
  }
  return true;
}

static bool validateControlStatus(const ReadyInspection &inspection) {
  if (!check(inspection.controlError.empty(), inspection.controlError)) {
    return false;
  }
  const auto response =
      pipetune::parseControlResponse(inspection.controlResponse);
  if (!check(response.valid, response.error) ||
      !check(response.success, "filter control status must succeed")) {
    return false;
  }
  const auto physical = physicalOutputs(inspection.graph);
  if (!check(response.status.filterOutputs.size() == physical.size(),
             "v2 status must report every physical output")) {
    return false;
  }
  for (const auto &output : response.status.filterOutputs) {
    if (!check(physical.contains(output.targetNodeName),
               "v2 status target must be a physical output") ||
        !check(output.state == pipetune::ControlFilterState::active ||
                   output.state ==
                       pipetune::ControlFilterState::bypassed,
               "ready output must be active or fail-open bypassed")) {
      return false;
    }
  }
  return true;
}

static void reportChildReady(void *userData) {
  const auto descriptor = *static_cast<int *>(userData);
  constexpr auto marker = char{'R'};
  auto result = ssize_t{-1};
  do {
    result = write(descriptor, &marker, 1);
  } while (result < 0 && errno == EINTR);
}

static bool stopServiceChild(pid_t child) {
  if (kill(child, SIGTERM) != 0) {
    return check(false, "cannot signal transparent-filter service child");
  }
  auto status = 0;
  auto waited = pid_t{-1};
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return check(waited == child && WIFEXITED(status) &&
                   WEXITSTATUS(status) == 0,
               "transparent-filter service child must stop orderly");
}

static bool responseHasPerOutputRate(
    const pipetune::ControlResponseParseResult &response,
    const pipetune::SampleRatePolicy &policy) {
  if (!response.valid || !response.success ||
      response.status.configuredRatePolicy != policy) {
    return false;
  }
  for (const auto &output : response.status.filterOutputs) {
    if (output.channelCount != 0 &&
        output.dspSampleRate != policy.fixedRate) {
      return false;
    }
  }
  return true;
}

static bool testLiveControl() {
  const auto processId = std::to_string(getpid());
  const auto directory = std::filesystem::temp_directory_path() /
                         ("pipetune-filter-live-" + processId);
  std::filesystem::create_directories(directory);
  const auto presetPath = directory / "live.effetune_preset";
  {
    auto preset = std::ofstream(presetPath, std::ios::binary);
    preset << R"json({"pipeline":[
      {"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}
    ]})json";
  }
  const auto socketPath = directory / "control.sock";
  auto backends = pipetune::discoverDspBackends();
  auto source = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 8, .maxFrames = 8192});
  auto descriptors = std::array<int, 2>{-1, -1};
  if (!check(source.pipeline != nullptr, source.error) ||
      !check(backends.scalar.backend != nullptr, backends.scalar.error) ||
      !check(pipe(descriptors.data()) == 0,
             "cannot create transparent-filter readiness pipe")) {
    std::filesystem::remove_all(directory);
    return false;
  }

  const auto child = fork();
  if (child < 0) {
    close(descriptors[0]);
    close(descriptors[1]);
    std::filesystem::remove_all(directory);
    return check(false, "cannot fork transparent-filter service test");
  }
  if (child == 0) {
    close(descriptors[0]);
    const auto result = pipetune::runPipeWireFilterService(
        std::move(source.pipeline),
        {.initialPresetPath = {},
         .initialConfigurationError = {},
         .controlSocketPath = socketPath,
         .ratePolicy = pipetune::defaultSampleRatePolicy(),
         .maxFrames = 8192,
         .ringCapacityFrames = 16384,
         .readyCallback = reportChildReady,
         .readyUserData = &descriptors[1],
         .dspBackends = std::move(backends),
         .configuredDspBackend = pipetune::DspBackendKind::scalar,
         .configuredDspSimdVariant =
             pipetune::DspSimdVariant::automatic},
        pipetune::PipeWireRunMode::untilInterrupted);
    if (!result.success) {
      std::cerr << result.error << '\n';
    }
    close(descriptors[1]);
    _exit(result.success ? 0 : 1);
  }

  close(descriptors[1]);
  auto marker = char{0};
  auto readResult = ssize_t{-1};
  do {
    readResult = read(descriptors[0], &marker, 1);
  } while (readResult < 0 && errno == EINTR);
  close(descriptors[0]);
  if (!check(readResult == 1 && marker == 'R',
             "transparent-filter child did not report readiness")) {
    static_cast<void>(stopServiceChild(child));
    std::filesystem::remove_all(directory);
    return false;
  }

  const auto load = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeLoadPresetControlRequest(presetPath));
  const auto loaded = pipetune::parseControlResponse(load.response);
  if (!check(load.error.empty(), load.error) ||
      !check(loaded.valid, loaded.error) ||
      !check(loaded.success &&
                 loaded.status.processingMode ==
                     pipetune::ProcessingMode::preset &&
                 loaded.status.activePreset == presetPath.string() &&
                 loaded.status.activePluginCount == 1,
             "live preset must replace every output pipeline")) {
    static_cast<void>(stopServiceChild(child));
    std::filesystem::remove_all(directory);
    return false;
  }

  const auto ratePolicy = pipetune::SampleRatePolicy{
      .mode = pipetune::SampleRateMode::fixed,
      .fixedRate = 96000,
      .enforcement = pipetune::SampleRateEnforcement::suggest};
  const auto rate = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeSetRateControlRequest(ratePolicy));
  const auto changedRate = pipetune::parseControlResponse(rate.response);
  if (!check(rate.error.empty(), rate.error) ||
      !check(responseHasPerOutputRate(changedRate, ratePolicy),
             "live rate policy must be resolved for every output")) {
    static_cast<void>(stopServiceChild(child));
    std::filesystem::remove_all(directory);
    return false;
  }

  if (backends.simd.backend != nullptr) {
    const auto backend = pipetune::exchangeControlMessage(
        socketPath, pipetune::makeSetDspBackendControlRequest(
                        pipetune::DspBackendKind::simd));
    const auto changedBackend =
        pipetune::parseControlResponse(backend.response);
    if (!check(backend.error.empty(), backend.error) ||
        !check(changedBackend.valid, changedBackend.error) ||
        !check(changedBackend.success &&
                   changedBackend.status.configuredDspBackend ==
                       pipetune::DspBackendKind::simd &&
                   changedBackend.status.effectiveDspBackend ==
                       pipetune::DspBackendKind::simd,
               "live backend must replace every preset pipeline")) {
      static_cast<void>(stopServiceChild(child));
      std::filesystem::remove_all(directory);
      return false;
    }
  }

  const auto missing = pipetune::exchangeControlMessage(
      socketPath,
      pipetune::makeLoadPresetControlRequest(directory /
                                             "missing.effetune_preset"));
  const auto afterFailure = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeStatusControlRequest());
  const auto retained =
      pipetune::parseControlResponse(afterFailure.response);
  if (!check(missing.error.empty(), missing.error) ||
      !check(!pipetune::inspectControlResponse(missing.response).success,
             "missing live preset must be rejected") ||
      !check(afterFailure.error.empty(), afterFailure.error) ||
      !check(retained.valid && retained.success &&
                 retained.status.activePreset == presetPath.string(),
             "failed live preset must retain every previous pipeline")) {
    static_cast<void>(stopServiceChild(child));
    std::filesystem::remove_all(directory);
    return false;
  }

  const auto bypass = pipetune::exchangeControlMessage(
      socketPath, pipetune::makeBypassControlRequest());
  const auto bypassed = pipetune::parseControlResponse(bypass.response);
  const auto bypassValid =
      check(bypass.error.empty(), bypass.error) &&
      check(bypassed.valid, bypassed.error) &&
      check(bypassed.success &&
                bypassed.status.processingMode ==
                    pipetune::ProcessingMode::bypass &&
                bypassed.status.activePreset.empty() &&
                bypassed.status.activePluginCount == 0,
            "live bypass must replace every output pipeline");
  const auto stopped = stopServiceChild(child);
  std::filesystem::remove_all(directory);
  return bypassValid && stopped;
}

int main() {
  if (!pipeWireSessionIsAvailable() || dumpPipeWireGraph() == std::nullopt) {
    std::cout << "PipeWire session or pw-dump is unavailable; skipping "
                 "filter service integration test\n";
    return 77;
  }

  auto source = pipetune::createBypassDspPipeline(
      {.sampleRate = 48000.0F, .maxChannels = 8, .maxFrames = 8192});
  if (!check(source.pipeline != nullptr, source.error)) {
    return 1;
  }
  auto inspection = ReadyInspection{};
  inspection.controlSocketPath =
      std::filesystem::temp_directory_path() /
      ("pipetune-filter-control-" + std::to_string(getpid()) + ".sock");
  const auto result = pipetune::runPipeWireFilterService(
      std::move(source.pipeline),
      {.initialPresetPath = {},
       .initialConfigurationError = {},
       .controlSocketPath = inspection.controlSocketPath,
       .ratePolicy = pipetune::defaultSampleRatePolicy(),
       .maxFrames = 8192,
       .ringCapacityFrames = 16384,
       .readyCallback = inspectReadyGraph,
       .readyUserData = &inspection},
      pipetune::PipeWireRunMode::untilReady);
  const auto graphValid = validateFilterPairs(inspection);
  const auto statusesValid = validateOutputStatuses(inspection, result);
  const auto controlValid = validateControlStatus(inspection);
  if (!graphValid || !statusesValid || !controlValid) {
    std::cerr << "service reported " << result.outputs.size()
              << " output runtimes\n";
    for (const auto &output : result.outputs) {
      std::cerr << "  runtime " << output.filterNodeName << " targets "
                << output.targetNodeName << ": " << output.error << '\n';
    }
  }
  return check(result.success, result.error) && graphValid && statusesValid &&
                 controlValid && testLiveControl()
             ? 0
             : 1;
}
