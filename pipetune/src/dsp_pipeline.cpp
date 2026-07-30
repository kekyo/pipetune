#include "pipetune/dsp_pipeline.h"

#include "dsp_backend_loader.h"
#include "dsp_catalog.h"

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pipetune {

constexpr auto kMaximumPresetBytes = std::uintmax_t{8u * 1024u * 1024u};
constexpr auto kMaximumInstances = std::size_t{96};
constexpr auto kPipelineDescriptorVersion = std::uint32_t{1};
constexpr auto kPipelineDescriptorHeaderBytes = std::size_t{8};
constexpr auto kPipelineDescriptorNodeBytes = std::size_t{12};

struct DspPipeline::Impl {
  std::shared_ptr<const DspBackend> backend;
  et_engine engine = 0;
  bool bypass = false;
  float sampleRate = 0.0F;
  std::uint32_t maxChannels = 0;
  std::uint32_t maxFrames = 0;
  std::uint32_t latencyFrames = 0;
  std::size_t activePluginCount = 0;
  std::shared_ptr<const std::string> presetRecipe;

  ~Impl() {
    if (engine != 0 && backend != nullptr) {
      dspBackendApi(*backend).engineDestroy(engine);
    }
  }
};

struct PresetNode {
  std::string_view name;
  bool enabled;
  yyjson_val *parameters;
  yyjson_val *inputBus;
  yyjson_val *outputBus;
  yyjson_val *channel;
};

struct ActiveNode {
  et_instance instance;
  std::uint8_t inputBus;
  std::uint8_t outputBus;
  std::int8_t channelSpec;
  std::uint32_t latency;
};

struct JsonDocumentDeleter {
  void operator()(yyjson_doc *document) const {
    yyjson_doc_free(document);
  }
};

using JsonDocument = std::unique_ptr<yyjson_doc, JsonDocumentDeleter>;

static PipelineLoadResult loadError(std::string message,
                                    std::vector<PipelineWarning> warnings = {}) {
  return {.pipeline = nullptr, .warnings = std::move(warnings), .error = std::move(message)};
}

static std::string validateBuildOptions(const PipelineBuildOptions &options) {
  if (!std::isfinite(options.sampleRate) || options.sampleRate < 32000.0F ||
      options.sampleRate > 384000.0F) {
    return "sample rate must be between 32000 and 384000 Hz";
  }
  if (options.maxChannels == 0 || options.maxChannels > 8) {
    return "maximum channel count must be between one and eight";
  }
  if (options.maxFrames < 32) {
    return "maximum frame count must be at least 32";
  }
  return {};
}

static std::string nodeError(std::size_t index, std::string_view message) {
  return "preset node " + std::to_string(index) + ": " + std::string(message);
}

static yyjson_val *objectMember(yyjson_val *object, std::string_view key) {
  if (!yyjson_is_obj(object)) {
    return nullptr;
  }
  return yyjson_obj_getn(object, key.data(), key.size());
}

static bool parseEnabled(yyjson_val *value, bool defaultValue, bool &output) {
  if (value == nullptr) {
    output = defaultValue;
    return true;
  }
  if (yyjson_is_bool(value)) {
    output = yyjson_get_bool(value);
    return true;
  }
  if (yyjson_is_num(value)) {
    const auto number = yyjson_get_num(value);
    if (number == 0.0 || number == 1.0) {
      output = number == 1.0;
      return true;
    }
  }
  return false;
}

static bool normalizeNode(yyjson_val *value, std::size_t index, PresetNode &node,
                          std::string &error) {
  if (!yyjson_is_obj(value)) {
    error = nodeError(index, "must be an object");
    return false;
  }
  auto *longName = objectMember(value, "name");
  auto *shortName = objectMember(value, "nm");
  const auto isLong = longName != nullptr;
  auto *name = isLong ? longName : shortName;
  if (!yyjson_is_str(name)) {
    error = nodeError(index, "must have a string name");
    return false;
  }
  node.name = std::string_view(yyjson_get_str(name), yyjson_get_len(name));

  auto *enabled = objectMember(value, isLong ? "enabled" : "en");
  if (!parseEnabled(enabled, true, node.enabled)) {
    error = nodeError(index, "enabled must be boolean or 0/1");
    return false;
  }

  if (isLong) {
    node.parameters = objectMember(value, "parameters");
    if (node.parameters != nullptr && !yyjson_is_obj(node.parameters)) {
      error = nodeError(index, "parameters must be an object");
      return false;
    }
    node.inputBus = objectMember(value, "inputBus");
    node.outputBus = objectMember(value, "outputBus");
    node.channel = objectMember(value, "channel");
    if (node.parameters != nullptr) {
      if (node.inputBus == nullptr) {
        node.inputBus = objectMember(node.parameters, "ib");
      }
      if (node.outputBus == nullptr) {
        node.outputBus = objectMember(node.parameters, "ob");
      }
      if (node.channel == nullptr) {
        node.channel = objectMember(node.parameters, "ch");
      }
    }
  } else {
    node.parameters = value;
    node.inputBus = objectMember(value, "ib");
    node.outputBus = objectMember(value, "ob");
    node.channel = objectMember(value, "ch");
  }
  return true;
}

static bool parseBus(yyjson_val *value, std::uint8_t &output) {
  if (value == nullptr) {
    output = 0;
    return true;
  }
  if (!yyjson_is_int(value)) {
    return false;
  }
  const auto number = yyjson_get_num(value);
  if (number < 0.0 || number > 4.0) {
    return false;
  }
  output = static_cast<std::uint8_t>(number);
  return true;
}

static bool parseChannel(yyjson_val *value, std::int8_t &output) {
  if (value == nullptr || yyjson_is_null(value)) {
    output = -1;
    return true;
  }
  if (!yyjson_is_str(value)) {
    return false;
  }
  const auto channel = std::string_view(yyjson_get_str(value), yyjson_get_len(value));
  if (channel.empty()) {
    output = -1;
    return true;
  }
  if (channel == "A" || channel == "All") {
    output = -2;
    return true;
  }
  if (channel == "L" || channel == "Left" || channel == "1") {
    output = 0;
    return true;
  }
  if (channel == "R" || channel == "Right" || channel == "2") {
    output = 1;
    return true;
  }
  if (channel.size() == 1 && channel[0] >= '3' && channel[0] <= '8') {
    output = static_cast<std::int8_t>(channel[0] - '1');
    return true;
  }
  if (channel == "34" || channel == "56" || channel == "78") {
    output = static_cast<std::int8_t>(16 + (channel[0] - '1') / 2);
    return true;
  }
  return false;
}

static void writeUint32(std::span<std::uint8_t> bytes, std::size_t offset, std::uint32_t value) {
  bytes[offset] = static_cast<std::uint8_t>(value & 0xffu);
  bytes[offset + 1] = static_cast<std::uint8_t>((value >> 8u) & 0xffu);
  bytes[offset + 2] = static_cast<std::uint8_t>((value >> 16u) & 0xffu);
  bytes[offset + 3] = static_cast<std::uint8_t>((value >> 24u) & 0xffu);
}

static std::vector<std::uint8_t> buildDescriptor(std::span<const ActiveNode> nodes) {
  auto descriptor = std::vector<std::uint8_t>(
      kPipelineDescriptorHeaderBytes + nodes.size() * kPipelineDescriptorNodeBytes, 0);
  writeUint32(descriptor, 0, kPipelineDescriptorVersion);
  writeUint32(descriptor, 4, static_cast<std::uint32_t>(nodes.size()));
  for (auto index = std::size_t{0}; index < nodes.size(); ++index) {
    const auto offset = kPipelineDescriptorHeaderBytes + index * kPipelineDescriptorNodeBytes;
    writeUint32(descriptor, offset, nodes[index].instance);
    descriptor[offset + 4] = 1;
    descriptor[offset + 5] = nodes[index].inputBus;
    descriptor[offset + 6] = nodes[index].outputBus;
    descriptor[offset + 7] = static_cast<std::uint8_t>(nodes[index].channelSpec);
    descriptor[offset + 8] = 1;
  }
  return descriptor;
}

static std::uint32_t calculateLatency(std::span<const ActiveNode> nodes) {
  auto busLatency = std::array<std::uint32_t, 5>{};
  for (const auto &node : nodes) {
    const auto inputLatency = busLatency[node.inputBus];
    const auto maximum = std::numeric_limits<std::uint32_t>::max();
    const auto routed =
        node.latency > maximum - inputLatency ? maximum : inputLatency + node.latency;
    if (node.inputBus == node.outputBus || routed > busLatency[node.outputBus]) {
      busLatency[node.outputBus] = routed;
    }
  }
  return busLatency[0];
}

static yyjson_val *findPipelineRoot(yyjson_val *root) {
  if (yyjson_is_arr(root)) {
    return root;
  }
  if (!yyjson_is_obj(root)) {
    return nullptr;
  }
  auto *pipeline = objectMember(root, "pipeline");
  if (pipeline != nullptr) {
    return yyjson_is_arr(pipeline) ? pipeline : nullptr;
  }
  auto *plugins = objectMember(root, "plugins");
  return yyjson_is_arr(plugins) ? plugins : nullptr;
}

DspPipeline::DspPipeline(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

DspPipeline::~DspPipeline() = default;
DspPipeline::DspPipeline(DspPipeline &&other) noexcept = default;
DspPipeline &DspPipeline::operator=(DspPipeline &&other) noexcept = default;

ProcessStatus DspPipeline::process(std::span<float> planarSamples, std::uint32_t channelCount,
                                   std::uint32_t frameCount, double timeSeconds) noexcept {
  if (implementation_ == nullptr || channelCount == 0 ||
      channelCount > implementation_->maxChannels || frameCount == 0 ||
      frameCount > implementation_->maxFrames || !std::isfinite(timeSeconds) ||
      planarSamples.size() != static_cast<std::size_t>(channelCount) * frameCount) {
    return ProcessStatus::invalidBuffer;
  }
  if (implementation_->bypass) {
    return ProcessStatus::ok;
  }
  const auto &api = dspBackendApi(*implementation_->backend);
  auto *arena = api.arenaCombinedPtr(implementation_->engine);
  if (arena == nullptr) {
    return ProcessStatus::dspError;
  }
  const auto byteCount = planarSamples.size_bytes();
  std::memcpy(arena, planarSamples.data(), byteCount);
  const auto status =
      api.pipelineProcess(implementation_->engine, channelCount, frameCount,
                          timeSeconds, 0);
  if (status != ET_OK) {
    return ProcessStatus::dspError;
  }
  std::memcpy(planarSamples.data(), arena, byteCount);
  return ProcessStatus::ok;
}

std::uint32_t DspPipeline::maxChannels() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->maxChannels;
}

float DspPipeline::sampleRate() const noexcept {
  return implementation_ == nullptr ? 0.0F : implementation_->sampleRate;
}

std::uint32_t DspPipeline::maxFrames() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->maxFrames;
}

std::uint32_t DspPipeline::latencyFrames() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->latencyFrames;
}

std::size_t DspPipeline::activePluginCount() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->activePluginCount;
}

std::optional<DspBackendKind> DspPipeline::backendKind() const noexcept {
  if (implementation_ == nullptr || implementation_->backend == nullptr) {
    return std::nullopt;
  }
  return implementation_->backend->kind();
}

std::optional<DspBackendVariant> DspPipeline::backendVariant() const noexcept {
  if (implementation_ == nullptr || implementation_->backend == nullptr) {
    return std::nullopt;
  }
  return implementation_->backend->variant();
}

bool DspPipeline::usesNativeDsp() const noexcept {
  return implementation_ != nullptr && !implementation_->bypass;
}

PipelineCreateResult
createBypassDspPipeline(const PipelineBuildOptions &options) {
  const auto validation = validateBuildOptions(options);
  if (!validation.empty()) {
    return {.pipeline = nullptr, .error = validation};
  }

  auto implementation = std::make_unique<DspPipeline::Impl>();
  implementation->bypass = true;
  implementation->sampleRate = options.sampleRate;
  implementation->maxChannels = options.maxChannels;
  implementation->maxFrames = options.maxFrames;
  auto pipeline =
      std::unique_ptr<DspPipeline>(new DspPipeline(std::move(implementation)));
  return {.pipeline = std::move(pipeline), .error = {}};
}

PipelineLoadResult DspPipeline::buildFromRecipe(
    std::shared_ptr<const std::string> presetRecipe,
    const PipelineBuildOptions &options,
    std::shared_ptr<const DspBackend> backend) {
  const auto validation = validateBuildOptions(options);
  if (!validation.empty()) {
    return loadError(validation);
  }
  if (presetRecipe == nullptr || presetRecipe->empty() ||
      presetRecipe->size() > kMaximumPresetBytes) {
    return loadError("retained preset recipe is unavailable");
  }
  if (backend == nullptr) {
    return loadError("DSP backend is unavailable");
  }
  auto document = JsonDocument(
      yyjson_read(presetRecipe->data(), presetRecipe->size(), 0));
  if (document == nullptr) {
    return loadError("cannot parse preset JSON");
  }
  auto *pipelineValue = findPipelineRoot(yyjson_doc_get_root(document.get()));
  if (pipelineValue == nullptr) {
    return loadError("preset root must contain a pipeline or plugins array");
  }

  auto implementation = std::make_unique<DspPipeline::Impl>();
  implementation->sampleRate = options.sampleRate;
  implementation->maxChannels = options.maxChannels;
  implementation->maxFrames = options.maxFrames;
  implementation->presetRecipe = std::move(presetRecipe);
  implementation->backend = std::move(backend);
  const auto &api = dspBackendApi(*implementation->backend);
  implementation->engine = api.engineCreate();
  if (implementation->engine == 0) {
    return loadError("cannot create EffeTune DSP engine");
  }
  const auto prepareStatus =
      api.enginePrepare(implementation->engine, options.sampleRate,
                        options.maxChannels, options.maxFrames, 0);
  if (prepareStatus != ET_OK) {
    return loadError("cannot prepare EffeTune DSP engine (status " +
                     std::to_string(prepareStatus) + ", handle " +
                     std::to_string(implementation->engine) + ", rate " +
                     std::to_string(options.sampleRate) + ", channels " +
                     std::to_string(options.maxChannels) + ", frames " +
                     std::to_string(options.maxFrames) + ", required " +
                     std::to_string(api.engineMemoryRequired(
                         options.sampleRate, options.maxChannels, options.maxFrames, 0)) +
                     ")");
  }

  auto warnings = std::vector<PipelineWarning>();
  auto activeNodes = std::vector<ActiveNode>();
  auto insideSection = false;
  auto sectionEnabled = true;
  const auto nodeCount = yyjson_arr_size(pipelineValue);
  for (auto index = std::size_t{0}; index < nodeCount; ++index) {
    auto node = PresetNode{};
    auto normalizationError = std::string{};
    if (!normalizeNode(yyjson_arr_get(pipelineValue, index), index, node, normalizationError)) {
      return loadError(std::move(normalizationError), std::move(warnings));
    }

    if (node.name == "Section") {
      insideSection = true;
      sectionEnabled = node.enabled;
      continue;
    }
    const auto *definition = findDspByDisplayName(node.name);
    if (definition == nullptr) {
      warnings.push_back(
          {.nodeIndex = index,
           .pluginName = std::string(node.name),
           .reason = "not available in EffeTune's native DSP registry"});
      continue;
    }
    if (definition->requiresExternalAssets) {
      warnings.push_back({.nodeIndex = index,
                          .pluginName = std::string(node.name),
                          .reason = "requires external asset loading"});
      continue;
    }
    if (!node.enabled || (insideSection && !sectionEnabled)) {
      continue;
    }
    if (activeNodes.size() >= kMaximumInstances) {
      return loadError("preset exceeds the native limit of 96 active DSP nodes",
                       std::move(warnings));
    }

    auto inputBus = std::uint8_t{0};
    auto outputBus = std::uint8_t{0};
    auto channelSpec = std::int8_t{-1};
    if (!parseBus(node.inputBus, inputBus) || !parseBus(node.outputBus, outputBus)) {
      return loadError(nodeError(index, "buses must be integers from zero through four"),
                       std::move(warnings));
    }
    if (!parseChannel(node.channel, channelSpec)) {
      return loadError(nodeError(index, "has an unsupported channel selection"),
                       std::move(warnings));
    }

    const auto instance =
        api.instanceCreate(implementation->engine, definition->typeName.data());
    if (instance == 0) {
      return loadError(nodeError(index, "cannot create native DSP instance"),
                       std::move(warnings));
    }
    auto packed = packDspParameters(*definition, node.parameters);
    if (!packed.error.empty()) {
      return loadError(nodeError(index, packed.error), std::move(warnings));
    }
    const auto floatStatus =
        api.instanceSetParams(
            implementation->engine, instance, packed.floats.data(),
            static_cast<std::uint32_t>(packed.floats.size()),
            definition->hash, 0);
    if (floatStatus != ET_OK) {
      return loadError(nodeError(index, "native DSP rejected packed parameters"),
                       std::move(warnings));
    }
    if (definition->structured.present) {
      const auto byteStatus = api.instanceSetParamBytes(
          implementation->engine, instance, packed.bytes.data(),
          static_cast<std::uint32_t>(packed.bytes.size()), definition->hash, 0);
      if (byteStatus != ET_OK) {
        return loadError(nodeError(index, "native DSP rejected structured parameters"),
                         std::move(warnings));
      }
    }
    activeNodes.push_back({.instance = instance,
                           .inputBus = inputBus,
                           .outputBus = outputBus,
                           .channelSpec = channelSpec,
                           .latency = api.instanceLatency(
                               implementation->engine, instance)});
  }

  const auto descriptor = buildDescriptor(activeNodes);
  const auto descriptorStatus = api.pipelineConfigure(
      implementation->engine, descriptor.data(), static_cast<std::uint32_t>(descriptor.size()));
  if (descriptorStatus != ET_OK) {
    return loadError("EffeTune rejected the native pipeline descriptor", std::move(warnings));
  }
  implementation->latencyFrames = calculateLatency(activeNodes);
  implementation->activePluginCount = activeNodes.size();

  auto pipeline = std::unique_ptr<DspPipeline>(new DspPipeline(std::move(implementation)));
  return {.pipeline = std::move(pipeline), .warnings = std::move(warnings), .error = {}};
}

PipelineLoadResult loadDspPipeline(const std::filesystem::path &presetPath,
                                   const PipelineBuildOptions &options) {
  auto backend = loadDspBackend(DspBackendKind::scalar);
  if (backend.backend == nullptr) {
    return loadError("scalar DSP backend is unavailable: " +
                     backend.error);
  }
  return loadDspPipeline(presetPath, options, std::move(backend.backend));
}

PipelineLoadResult
loadDspPipeline(const std::filesystem::path &presetPath,
                const PipelineBuildOptions &options,
                std::shared_ptr<const DspBackend> backend) {
  if (presetPath.extension() != ".effetune_preset") {
    return loadError("preset path must use the exact .effetune_preset extension");
  }
  const auto validation = validateBuildOptions(options);
  if (!validation.empty()) {
    return loadError(validation);
  }

  auto fileError = std::error_code{};
  const auto fileBytes = std::filesystem::file_size(presetPath, fileError);
  if (fileError) {
    return loadError("cannot inspect preset file: " + fileError.message());
  }
  if (fileBytes == 0 || fileBytes > kMaximumPresetBytes) {
    return loadError("preset file must contain between 1 byte and 8 MiB");
  }
  auto stream = std::ifstream(presetPath, std::ios::binary);
  if (!stream) {
    return loadError("cannot read preset file");
  }
  auto contents =
      std::string(static_cast<std::size_t>(fileBytes), '\0');
  stream.read(contents.data(),
              static_cast<std::streamsize>(contents.size()));
  if (stream.gcount() !=
          static_cast<std::streamsize>(contents.size()) ||
      stream.bad()) {
    return loadError("cannot read preset file");
  }
  return DspPipeline::buildFromRecipe(
      std::make_shared<const std::string>(std::move(contents)), options,
      std::move(backend));
}

PipelineLoadResult
rebuildDspPipeline(const DspPipeline &source,
                   const PipelineBuildOptions &options) {
  if (source.implementation_ == nullptr) {
    return loadError("source DSP pipeline is unavailable");
  }
  return rebuildDspPipeline(source, options,
                            source.implementation_->backend);
}

PipelineLoadResult
rebuildDspPipeline(const DspPipeline &source,
                   const PipelineBuildOptions &options,
                   std::shared_ptr<const DspBackend> backend) {
  if (source.implementation_ == nullptr) {
    return loadError("source DSP pipeline is unavailable");
  }
  if (source.implementation_->bypass) {
    auto created = createBypassDspPipeline(options);
    return {.pipeline = std::move(created.pipeline),
            .warnings = {},
            .error = std::move(created.error)};
  }
  return DspPipeline::buildFromRecipe(
      source.implementation_->presetRecipe, options, std::move(backend));
}

} // namespace pipetune
