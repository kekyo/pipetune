/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/dsp_pipeline.h"

#include "dsp_backend_loader.h"
#include "bass_management_config.h"
#include "dsp_catalog.h"
#include "generated_fir_asset.h"
#include "ir_asset_loader.h"
#include "sfz_asset_loader.h"
#include "room_asset_loader.h"
#include "asset_file.h"
#include "preparation_dispatcher.h"
#include "measurement_store.h"
#include <cstdlib>

#include <yyjson.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iterator>
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

struct SfzEventInstance {
  et_instance instance;
  std::uint32_t channels;
  std::uint32_t hash;
  std::vector<float> parameters;
  std::size_t previewIndex;
};

// Event detection suppresses note starts in its first analysis frame. Seed that
// frame with silence so the first input after preparation or reset is admitted.
static et_status primeSfzEvent(const DspBackendApi &api, et_engine engine,
    SfzEventInstance &sfz, float sampleRate, std::uint32_t maxFrames) noexcept {
  auto *silence = api.arenaCombinedPtr(engine);
  if (silence == nullptr) return ET_ERR_STATE;
  const auto preview = sfz.parameters[sfz.previewIndex];
  if (preview >= 0.0F) {
    // Keep preview playback at its first sample while priming only the analysis.
    sfz.parameters[sfz.previewIndex] = -1.0F;
    const auto status = api.instanceSetParams(engine, sfz.instance,
        sfz.parameters.data(), static_cast<std::uint32_t>(sfz.parameters.size()), sfz.hash, 0u);
    sfz.parameters[sfz.previewIndex] = preview;
    if (status != ET_OK) return status;
  }
  // Match SFZ's 20 ms analysis hop, including the upstream frame bounds.
  const auto hop = std::clamp(static_cast<std::uint32_t>(std::lround(sampleRate * 0.02)), 16u, 8192u);
  auto status = et_status{ET_OK};
  for (auto offset = 0u; offset < hop; ) {
    const auto frames = std::min(maxFrames, hop - offset);
    std::fill_n(silence, sfz.channels * frames, 0.0F);
    status = api.instanceProcess(engine, sfz.instance, silence, sfz.channels,
        frames, offset / static_cast<double>(sampleRate));
    if (status != ET_OK) break;
    offset += frames;
  }
  if (preview >= 0.0F) {
    const auto restored = api.instanceSetParams(engine, sfz.instance,
        sfz.parameters.data(), static_cast<std::uint32_t>(sfz.parameters.size()), sfz.hash, 0u);
    if (status == ET_OK) status = restored;
  }
  return status;
}

struct DspPipeline::Impl {
  std::shared_ptr<const DspBackend> backend;
  et_engine engine = 0;
  bool bypass = false;
  float sampleRate = 0.0F;
  std::uint32_t maxChannels = 0;
  std::uint32_t outputChannelCount = 0;
  std::uint32_t maxFrames = 0;
  std::uint32_t latencyFrames = 0;
  std::size_t activePluginCount = 0;
  std::vector<PresetEntry> presetEntries;
  std::shared_ptr<const std::string> presetRecipe;
  PipelineLoadContext loadContext;
  std::vector<std::filesystem::path> dependencyFiles;
  std::vector<std::shared_ptr<const AssetCachePin>> assetCachePins;
  std::vector<AssetMemoryReservation> nativeAssetMemory;
  std::vector<SfzEventInstance> sfzEventInstances;

  ~Impl() {
    if (engine != 0 && backend != nullptr) {
      dspBackendApi(*backend).engineDestroy(engine);
    }
  }
};

PipelineLoadContext defaultPipelineLoadContext(std::uint32_t sfzMaxSizeMiB) {
  if (!sfzMaxSizeMiBIsValid(sfzMaxSizeMiB)) throw std::invalid_argument("SFZ budget is invalid");
  const auto *xdg = std::getenv("XDG_CONFIG_HOME");
  const auto *home = std::getenv("HOME");
  const auto directory = resolveEffeTuneDirectory(xdg == nullptr ? "" : xdg, home == nullptr ? "" : home);
  const auto *xdgCache = std::getenv("XDG_CACHE_HOME");
  auto cache = xdgCache != nullptr && *xdgCache != '\0' && std::filesystem::path(xdgCache).is_absolute() ?
      std::filesystem::path(xdgCache) : home != nullptr && *home != '\0' ? std::filesystem::path(home) / ".cache" : std::filesystem::path{};
  if (!cache.empty()) cache /= "pipetune";
  return {.measurementDirectory = directory.empty() ? std::filesystem::path{} : directory / "measurement-backups",
          .effetuneDirectory = directory, .assetCache = {.directory = std::move(cache)},
          .sfzMaximumBytes = std::uint64_t{sfzMaxSizeMiB} * 1024 * 1024};
}

std::span<const std::filesystem::path> DspPipeline::dependencyFiles() const noexcept {
  return implementation_->dependencyFiles;
}

std::span<const PresetEntry> DspPipeline::presetEntries() const noexcept {
  return implementation_->presetEntries;
}

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
};

struct JsonDocumentDeleter {
  void operator()(yyjson_doc *document) const {
    yyjson_doc_free(document);
  }
};

using JsonDocument = std::unique_ptr<yyjson_doc, JsonDocumentDeleter>;

static PipelineLoadResult loadError(std::string message,
                                    std::vector<PipelineWarning> warnings = {},
                                    std::vector<std::filesystem::path> files = {}) {
  return {.pipeline = nullptr, .warnings = std::move(warnings),
          .dependencyFiles = std::move(files), .error = std::move(message)};
}

static std::string validateBuildOptions(const PipelineBuildOptions &options) {
  if (!std::isfinite(options.sampleRate) || options.sampleRate < 32000.0F ||
      options.sampleRate > 384000.0F) {
    return "sample rate must be between 32000 and 384000 Hz";
  }
  if (options.maxChannels == 0 || options.maxChannels > 16) {
    return "maximum channel count must be between one and sixteen";
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
  auto channelNumber = std::uint32_t{0};
  const auto parsed = std::from_chars(
      channel.data(), channel.data() + channel.size(), channelNumber);
  if (parsed.ec == std::errc{} &&
      parsed.ptr == channel.data() + channel.size() &&
      channelNumber >= 3u && channelNumber <= 16u) {
    output = static_cast<std::int8_t>(channelNumber - 1u);
    return true;
  }
  static constexpr auto channelPairs = std::array<std::string_view, 7>{
      "34", "56", "78", "910", "1112", "1314", "1516"};
  const auto pair = std::ranges::find(channelPairs, channel);
  if (pair != channelPairs.end()) {
    output = static_cast<std::int8_t>(
        17u + static_cast<std::uint32_t>(
                  std::distance(channelPairs.begin(), pair)));
    return true;
  }
  return false;
}

static std::uint32_t selectedProcessingChannels(std::int8_t channelSpec,
                                                std::uint32_t maxChannels) {
  if (channelSpec == -2) {
    return maxChannels;
  }
  if (channelSpec == -1 || channelSpec >= 16) {
    return std::min(maxChannels, 2u);
  }
  return 1u;
}

struct PresetChannelLayout {
  std::uint32_t processingChannels = 2;
  std::uint32_t outputChannels = 2;
};

struct LayoutNode {
  std::string_view name;
  std::int8_t channelSpec;
  std::uint8_t inputBus;
  std::uint8_t outputBus;
  std::uint32_t bandCount = 0;
  std::uint32_t bassMask = 0;
  std::uint32_t spatialInputChannels = 0;
  std::uint32_t spatialMask = 0;
  std::vector<std::uint8_t> matrixRoutes;
};

static std::uint32_t layoutParameter(const PackedParameters &packed, std::string_view key,
    std::string_view arrayKey = {}, std::size_t index = 0) {
  for (auto i = std::size_t{0}; i < packed.floats.size(); ++i) {
    const auto &element = packed.definition->elements[i];
    if ((!arrayKey.empty() && element.arrayKey == arrayKey && element.containerIndex == index) ||
        (arrayKey.empty() && element.directKey == key))
      return static_cast<std::uint32_t>(std::max(0.0F, packed.floats[i]));
  }
  return 0;
}

static std::uint32_t channelSpecWidth(std::int8_t spec) {
  if (spec >= 16) return static_cast<std::uint32_t>(spec - 16) * 2u + 2u;
  return spec >= 0 ? static_cast<std::uint32_t>(spec) + 1u : 2u;
}

static std::uint32_t bitWidth(std::uint32_t mask) {
  return mask == 0 ? 0u : 32u - static_cast<std::uint32_t>(std::countl_zero(mask));
}

static PresetChannelLayout inferPresetChannelLayout(yyjson_val *pipelineValue) {
  auto nodes = std::vector<LayoutNode>{};
  auto layout = PresetChannelLayout{};
  auto needsStereoBands = false;
  auto insideSection = false;
  auto sectionEnabled = true;
  const auto count = yyjson_arr_size(pipelineValue);
  for (auto index = std::size_t{0}; index < count; ++index) {
    auto node = PresetNode{};
    auto error = std::string{};
    if (!normalizeNode(yyjson_arr_get(pipelineValue, index), index, node, error)) continue;
    if (node.name == "Section") {
      insideSection = true;
      sectionEnabled = node.enabled;
      continue;
    }
    const auto *definition = findDspByDisplayName(node.name);
    if (!node.enabled || (insideSection && !sectionEnabled) || definition == nullptr ||
        definition->visualizationOnly ||
        (definition->requiresExternalAssets &&
         !(node.name == "IR Reverb" || node.name == "SFZ Note Player" ||
           node.name == "Room EQ" || node.name == "Bass Management" ||
           node.name == "Crosstalk Cancellation" || supportsGeneratedFirAsset(node.name)))) continue;
    auto inputBus = std::uint8_t{0};
    auto outputBus = std::uint8_t{0};
    auto channelSpec = std::int8_t{-1};
    if (!parseBus(node.inputBus, inputBus) || !parseBus(node.outputBus, outputBus) ||
        !parseChannel(node.channel, channelSpec)) continue;
    auto packed = packDspParameters(*definition, node.parameters);
    if (!packed.error.empty()) continue;
    auto item = LayoutNode{.name = node.name, .channelSpec = channelSpec,
        .inputBus = inputBus, .outputBus = outputBus, .bandCount = 0,
        .bassMask = 0, .spatialInputChannels = 0, .spatialMask = 0, .matrixRoutes = {}};
    layout.processingChannels = std::max(layout.processingChannels, channelSpecWidth(channelSpec));
    if (node.name == "Matrix") {
      item.matrixRoutes = std::move(packed.bytes);
      for (auto offset = std::size_t{4}; offset + 2 < item.matrixRoutes.size(); offset += 3) {
        const auto routeWidth = std::max(item.matrixRoutes[offset], item.matrixRoutes[offset + 1]) + 1u;
        if (channelSpec == -2) layout.processingChannels = std::max(layout.processingChannels, routeWidth);
      }
    } else if (node.name == "Channel Divider" || node.name == "FIR Crossover") {
      item.bandCount = std::clamp(layoutParameter(packed, "bc"), 2u, 4u);
      if (channelSpec == -2) {
        needsStereoBands = true;
        layout.processingChannels = std::max(layout.processingChannels, item.bandCount * 2u);
      }
    } else if (node.name == "Spatial Mapper") {
      const auto localWidth = selectedProcessingChannels(channelSpec, 16u);
      item.spatialInputChannels = std::min(localWidth, std::clamp(layoutParameter(packed, "ic"), 1u, 16u));
      for (auto parameter = std::size_t{0}; parameter < packed.floats.size(); ++parameter) {
        const auto &element = packed.definition->elements[parameter];
        if (element.arrayKey != "dm" && element.arrayKey != "fm" && element.arrayKey != "rm") continue;
        const auto input = element.containerIndex % 16u;
        const auto output = element.containerIndex / 16u;
        if (input < item.spatialInputChannels && output < localWidth && packed.floats[parameter] != 0.0F)
          item.spatialMask |= 1u << output;
      }
      if (channelSpec == -2) layout.processingChannels = std::max(layout.processingChannels,
          std::max(item.spatialInputChannels, bitWidth(item.spatialMask)));
    } else if (node.name == "Bass Management") {
      item.bassMask = layoutParameter(packed, "su");
      for (auto channel = std::size_t{0}; channel < 16; ++channel) {
        const auto role = layoutParameter(packed, {}, "ro", channel);
        const auto routes = layoutParameter(packed, {}, "rt", channel);
        item.bassMask |= routes;
        if (role == 1u || role == 2u || routes != 0u)
          layout.processingChannels = std::max(layout.processingChannels,
              static_cast<std::uint32_t>(channel + 1u));
      }
      layout.processingChannels = std::max(layout.processingChannels, bitWidth(item.bassMask));
    }
    nodes.push_back(std::move(item));
  }
  // The band splitters require stereo pairs even when a later node addresses
  // an odd final channel. The padding channel is not itself a preset output.
  if (needsStereoBands) layout.processingChannels += layout.processingChannels % 2u;
  layout.processingChannels = std::min(layout.processingChannels, 16u);

  auto busMasks = std::array<std::uint32_t, 5>{3u, 0u, 0u, 0u, 0u};
  for (const auto &node : nodes) {
    const auto source = busMasks[node.inputBus];
    const auto first = node.channelSpec >= 16 ?
        static_cast<std::uint32_t>(node.channelSpec - 16) * 2u :
        node.channelSpec >= 0 ? static_cast<std::uint32_t>(node.channelSpec) : 0u;
    const auto selection = node.channelSpec == -2 ?
        ((1u << layout.processingChannels) - 1u) :
        ((1u << selectedProcessingChannels(node.channelSpec, layout.processingChannels)) - 1u) << first;
    auto result = source;
    if (node.name == "Matrix") {
      result = source & ~selection;
      for (auto offset = std::size_t{4}; offset + 2 < node.matrixRoutes.size(); offset += 3) {
        const auto localWidth = selectedProcessingChannels(node.channelSpec, layout.processingChannels);
        if (node.matrixRoutes[offset] >= localWidth || node.matrixRoutes[offset + 1] >= localWidth)
          continue;
        const auto input = first + node.matrixRoutes[offset];
        const auto output = first + node.matrixRoutes[offset + 1];
        if (input < layout.processingChannels && output < layout.processingChannels &&
            (selection & (1u << input)) != 0)
          result |= 1u << output;
      }
    } else if (node.spatialInputChannels != 0) {
      // Spatial Mapper replaces its selected input lanes. Higher lanes without
      // a matrix destination retain their dry input in the native processor.
      const auto inputs = ((1u << node.spatialInputChannels) - 1u) << first;
      result = (source & ~inputs) | (node.spatialMask << first);
    } else if (node.bandCount != 0 && node.channelSpec == -2) {
      // Both splitters replace the full bus and clear lanes beyond their bands.
      result = (1u << (node.bandCount * 2u)) - 1u;
    } else if (node.name == "Bass Management" && source != 0) {
      result |= node.bassMask;
    } else if (node.name == "DC Offset" || node.name == "Hum Generator") {
      result |= selection;
    } else if (node.name == "Oscillator" || node.name == "SFZ Note Player") {
      result |= node.channelSpec == -2 ? 3u : selection;
    } else if (node.channelSpec >= 0) {
      // An explicit channel address declares a bus lane even when its input is silent.
      result |= selection;
    }
    if (node.inputBus == node.outputBus) busMasks[node.outputBus] = result;
    else busMasks[node.outputBus] |= result & selection;
  }
  layout.outputChannels = std::clamp(bitWidth(busMasks[0]), 1u, 16u);
  return layout;
}

static bool replacePackedParameter(PackedParameters &packed,
                                   std::string_view key, float value) {
  const auto elements = packed.definition->elements;
  const auto match = std::ranges::find(elements, key,
                                       &ParameterElement::directKey);
  if (match == elements.end()) {
    return false;
  }
  packed.floats[static_cast<std::size_t>(
      std::distance(elements.begin(), match))] = value;
  return true;
}

static float packedHeadBlock(std::uint32_t headBlock) {
  switch (headBlock) {
  case 0u:
    return 0.0F;
  case 128u:
    return 1.0F;
  case 256u:
    return 2.0F;
  case 512u:
    return 3.0F;
  case 1024u:
    return 4.0F;
  default:
    return 1.0F;
  }
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

ProcessStatus DspPipeline::reset() noexcept {
  if (implementation_ == nullptr) {
    return ProcessStatus::dspError;
  }
  if (implementation_->bypass) {
    return ProcessStatus::ok;
  }
  const auto &api = dspBackendApi(*implementation_->backend);
  if (api.engineReset(implementation_->engine) != ET_OK) return ProcessStatus::dspError;
  for (auto &sfz : implementation_->sfzEventInstances)
    if (primeSfzEvent(api, implementation_->engine, sfz,
            implementation_->sampleRate, implementation_->maxFrames) != ET_OK)
      return ProcessStatus::dspError;
  return ProcessStatus::ok;
}

std::uint32_t DspPipeline::maxChannels() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->maxChannels;
}

std::uint32_t DspPipeline::outputChannelCount() const noexcept {
  return implementation_ == nullptr ? 0 : implementation_->outputChannelCount;
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
  implementation->maxChannels = options.inferChannelsFromPreset ? 2u : options.maxChannels;
  implementation->outputChannelCount = implementation->maxChannels;
  implementation->maxFrames = options.maxFrames;
  auto pipeline =
      std::unique_ptr<DspPipeline>(new DspPipeline(std::move(implementation)));
  return {.pipeline = std::move(pipeline), .error = {}};
}

cardio::promise<PipelineLoadResult> DspPipeline::buildFromRecipe(
    std::shared_ptr<const std::string> presetRecipe,
    PipelineBuildOptions options,
    std::shared_ptr<const DspBackend> backend,
    PipelineLoadContext context, cardio::cancellation cancellation) {
  co_await preparationCheckpoint(cancellation);
  const auto validation = validateBuildOptions(options);
  if (!validation.empty()) {
    co_return loadError(validation);
  }
  if (presetRecipe == nullptr || presetRecipe->empty() ||
      presetRecipe->size() > kMaximumPresetBytes) {
    co_return loadError("retained preset recipe is unavailable");
  }
  if (backend == nullptr) {
    co_return loadError("DSP backend is unavailable");
  }
  auto document = JsonDocument(
      yyjson_read(presetRecipe->data(), presetRecipe->size(), 0));
  if (document == nullptr) {
    co_return loadError("cannot parse preset JSON");
  }
  auto *pipelineValue = findPipelineRoot(yyjson_doc_get_root(document.get()));
  if (pipelineValue == nullptr) {
    co_return loadError("preset root must contain a pipeline or plugins array");
  }

  const auto layout = options.inferChannelsFromPreset ? inferPresetChannelLayout(pipelineValue) :
      PresetChannelLayout{options.maxChannels, options.maxChannels};
  options.maxChannels = layout.processingChannels;

  auto implementation = std::make_unique<DspPipeline::Impl>();
  const auto fail = [&implementation](std::string message, std::vector<PipelineWarning> warnings = {}) {
    return loadError(std::move(message), std::move(warnings), implementation->dependencyFiles);
  };
  implementation->sampleRate = options.sampleRate;
  implementation->maxChannels = options.maxChannels;
  implementation->outputChannelCount = layout.outputChannels;
  implementation->maxFrames = options.maxFrames;
  implementation->loadContext = context;
  auto measurementStore = MeasurementStore{context.measurementDirectory, {}};
  implementation->presetRecipe = std::move(presetRecipe);
  implementation->backend = std::move(backend);
  const auto &api = dspBackendApi(*implementation->backend);
  implementation->engine = api.engineCreate();
  if (implementation->engine == 0) {
    co_return fail("cannot create EffeTune DSP engine");
  }
  const auto prepareStatus =
      api.enginePrepare(implementation->engine, options.sampleRate,
                        options.maxChannels, options.maxFrames, 0);
  if (prepareStatus != ET_OK) {
    co_return fail("cannot prepare EffeTune DSP engine (status " +
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
  auto cachedAssetCount = std::size_t{0};
  auto preparedAssets = std::vector<std::shared_ptr<const SharedPreparedAsset>>{};
  auto sourceSnapshots = std::vector<AssetFileSnapshot>{};
  auto insideSection = false;
  auto sectionEnabled = true;
  const auto nodeCount = yyjson_arr_size(pipelineValue);
  for (auto index = std::size_t{0}; index < nodeCount; ++index) {
    co_await preparationCheckpoint(cancellation);
    auto node = PresetNode{};
    auto normalizationError = std::string{};
    if (!normalizeNode(yyjson_arr_get(pipelineValue, index), index, node, normalizationError)) {
      co_return fail(std::move(normalizationError), std::move(warnings));
    }

    auto &entry = implementation->presetEntries.emplace_back(
        PresetEntry{std::string(node.name), PresetEntryState::ignored});
    if (node.name == "Section") {
      insideSection = true;
      sectionEnabled = node.enabled;
      entry.state = node.enabled ? PresetEntryState::enabled : PresetEntryState::off;
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
    if (definition->visualizationOnly) {
      continue;
    }
    if (!node.enabled || (insideSection && !sectionEnabled)) {
      entry.state = PresetEntryState::off;
      continue;
    }
    const auto crosstalk = node.name == "Crosstalk Cancellation";
    auto measured = CrosstalkMeasurements{};
    if (crosstalk) {
      auto ids = std::array<std::string, 4>{};
      constexpr auto keys = std::array{"ll", "lr", "rl", "rr"};
      for (std::size_t slot = 0; slot < keys.size(); ++slot) {
        auto *value = yyjson_obj_get(node.parameters, keys[slot]);
        if (yyjson_is_str(value)) ids[slot].assign(yyjson_get_str(value), yyjson_get_len(value));
      }
      measured = std::move(co_await loadCrosstalkMeasurementsAsync(measurementStore, ids, cancellation));
      for (const auto &file : measured.files) {
        if (std::ranges::find(implementation->dependencyFiles, file) == implementation->dependencyFiles.end()) implementation->dependencyFiles.push_back(file);
      }
      if (!measured.error.empty()) {
        warnings.push_back({.nodeIndex = index, .pluginName = std::string(node.name), .reason = measured.error});
        continue;
      }
      for (const auto &[id, snapshot] : measurementStore.snapshots) sourceSnapshots.push_back(snapshot);
    }
    const auto bassManagement = node.name == "Bass Management";
    const auto irReverb = node.name == "IR Reverb";
    const auto sfzPlayer = node.name == "SFZ Note Player";
    const auto roomEq = node.name == "Room EQ";
    const auto generatedAssetDsp = irReverb || sfzPlayer || roomEq || bassManagement || crosstalk || supportsGeneratedFirAsset(node.name);
    if (definition->requiresExternalAssets && !generatedAssetDsp) {
      warnings.push_back({.nodeIndex = index,
                          .pluginName = std::string(node.name),
                          .reason = "requires external asset loading"});
      continue;
    }
    if (activeNodes.size() >= kMaximumInstances) {
      co_return fail("preset exceeds the native limit of 96 active DSP nodes",
                       std::move(warnings));
    }

    auto inputBus = std::uint8_t{0};
    auto outputBus = std::uint8_t{0};
    auto channelSpec = std::int8_t{-1};
    if (!parseBus(node.inputBus, inputBus) || !parseBus(node.outputBus, outputBus)) {
      co_return fail(nodeError(index, "buses must be integers from zero through four"),
                       std::move(warnings));
    }
    if (!parseChannel(node.channel, channelSpec)) {
      co_return fail(nodeError(index, "has an unsupported channel selection"),
                       std::move(warnings));
    }

    if ((node.name == "Bass Extender" || node.name == "Adaptive Prediction") &&
        selectedProcessingChannels(channelSpec, options.maxChannels) > 2u) {
      co_return fail(
          nodeError(index, std::string(node.name) + " requires one or two selected processing channels"),
          std::move(warnings));
    }

    auto packed = packDspParameters(*definition, node.parameters);
    if (!packed.error.empty()) {
      co_return fail(nodeError(index, packed.error), std::move(warnings));
    }
    auto bassConfig = BassManagementConfig{};
    if (bassManagement) {
      if (channelSpec != -2) {
        co_return fail(nodeError(index, "Bass Management requires an explicit All channel selection"),
                         std::move(warnings));
      }
      bassConfig = decodeBassManagementConfig(packed, options.maxChannels);
      if (!bassConfig.error.empty()) co_return fail(nodeError(index, bassConfig.error), std::move(warnings));
    }
    auto generatedMemory = AssetMemoryReservation{};
    auto ownedAsset = PreparedDspAsset{};
    auto sharedAsset = std::shared_ptr<const SharedPreparedAsset>{};
    if (generatedAssetDsp) {
      if (!irReverb && !sfzPlayer && !roomEq) {
        try { generatedMemory = AssetMemoryReservation(512ull * 1024 * 1024, context.assetMemoryBytes); }
        catch (const std::exception &error) { co_return fail(nodeError(index, error.what()), std::move(warnings)); }
      }
      const auto processingChannels =
          selectedProcessingChannels(channelSpec, options.maxChannels);
      if (irReverb || sfzPlayer || roomEq) {
        const auto firstChannel = channelSpec >= 16 ? static_cast<std::uint32_t>(channelSpec - 16) * 2u :
                                  channelSpec >= 0 ? static_cast<std::uint32_t>(channelSpec) : 0u;
        const auto availableChannels = firstChannel < options.maxChannels &&
            processingChannels <= options.maxChannels - firstChannel ? processingChannels : 0u;
        const auto collect = [&](auto &loaded) {
          if (loaded.cacheHit) ++cachedAssetCount;
          for (const auto &file : loaded.files)
            if (std::ranges::find(implementation->dependencyFiles, file) == implementation->dependencyFiles.end())
              implementation->dependencyFiles.push_back(file);
          for (auto &snapshot : loaded.snapshots) sourceSnapshots.push_back(std::move(snapshot));
          sharedAsset = std::move(loaded.prepared);
          return loaded.error;
        };
        auto error = std::string{};
        if (irReverb) {
          auto loaded = std::move(co_await loadIrAssetAsync(context.effetuneDirectory, node.parameters,
              options.sampleRate, availableChannels, context.assetCache, cancellation, context.assetMemoryBytes));
          error = collect(loaded);
        } else if (sfzPlayer) {
          if (!availableChannels) co_return fail(nodeError(index, "selected SFZ processing channels are unavailable"), std::move(warnings));
          auto loaded = std::move(co_await loadSfzAssetAsync(context.effetuneDirectory, node.parameters,
              context.sfzMaximumBytes, context.assetCache, cancellation, context.assetMemoryBytes));
          error = collect(loaded);
        } else {
          auto loaded = std::move(co_await loadRoomAssetAsync(context.measurementDirectory, node.parameters,
              options.sampleRate, availableChannels, context.assetCache, cancellation, context.assetMemoryBytes));
          if (loaded.error.empty() && !replacePackedParameter(packed, "dy", static_cast<float>(loaded.channelDelaySamples)))
            co_return fail(nodeError(index, "Room EQ delay does not match the native catalog"), std::move(warnings));
          error = collect(loaded);
        }
        if (!error.empty()) co_return fail(nodeError(index, error), std::move(warnings));
        preparedAssets.push_back(sharedAsset);
        if (sharedAsset->cachePin) implementation->assetCachePins.push_back(sharedAsset->cachePin);
        for (const auto &diagnostic : sharedAsset->diagnostics)
          warnings.push_back({index, std::string(node.name), diagnostic, PresetEntryState::enabled});
      } else if (bassManagement) {
        ownedAsset = designBassManagementAsset(
            bassConfig, options.sampleRate, options.maxFrames, api);
      } else if (crosstalk) {
        ownedAsset = designCrosstalkAsset(
            measured, node.parameters, options.sampleRate, processingChannels);
      } else {
        ownedAsset = designGeneratedFirAsset(
            node.name, node.parameters, options.sampleRate, processingChannels, api);
      }
      generatedMemory.shrink(ownedAsset.payload.capacity());
      const auto &generatedAsset = sharedAsset ? sharedAsset->asset : ownedAsset;
      if (!generatedAsset.omissionReason.empty()) {
        warnings.push_back({.nodeIndex = index,
                            .pluginName = std::string(node.name),
                            .reason = generatedAsset.omissionReason});
        continue;
      }
      if (!generatedAsset.error.empty()) {
        co_return fail(nodeError(index, generatedAsset.error),
                         std::move(warnings));
      }
      if (!bassManagement && !irReverb && !sfzPlayer && !generatedAsset.payload.empty() &&
          (!replacePackedParameter(
               packed, "lt", packedHeadBlock(generatedAsset.info.head_block)) ||
           !replacePackedParameter(
               packed, "fd",
               static_cast<float>(generatedAsset.filterDelaySamples)) ||
           (node.name == "FIR Crossover" &&
            !replacePackedParameter(
                packed, "bc", static_cast<float>(generatedAsset.bandCount))))) {
        co_return fail(
            nodeError(index, "generated FIR parameters do not match the native catalog"),
            std::move(warnings));
      }
    }

    const auto &generatedAsset = sharedAsset ? sharedAsset->asset : ownedAsset;

    const auto instance =
        api.instanceCreate(implementation->engine, definition->typeName.data());
    if (instance == 0) {
      co_return fail(nodeError(index, "cannot create native DSP instance"),
                       std::move(warnings));
    }
    const auto floatStatus =
        api.instanceSetParams(
            implementation->engine, instance, packed.floats.data(),
            static_cast<std::uint32_t>(packed.floats.size()),
            definition->hash, 0);
    if (floatStatus != ET_OK) {
      co_return fail(nodeError(index, "native DSP rejected packed parameters"),
                       std::move(warnings));
    }
    if (definition->structured.present) {
      const auto byteStatus = api.instanceSetParamBytes(
          implementation->engine, instance, packed.bytes.data(),
          static_cast<std::uint32_t>(packed.bytes.size()), definition->hash, 0);
      if (byteStatus != ET_OK) {
        co_return fail(nodeError(index, "native DSP rejected structured parameters"),
                         std::move(warnings));
      }
    }
    if (generatedAssetDsp && !generatedAsset.payload.empty()) {
      try {
        // The footprint includes the native staging payload and convolution
        // buffers. Keep the admission probe's 1 MiB margin through warmup.
        implementation->nativeAssetMemory.emplace_back(
            static_cast<std::uint64_t>(generatedAsset.info.footprint_bytes) + 1024 * 1024,
            context.assetMemoryBytes);
      } catch (const std::exception &error) {
        co_return fail(nodeError(index, error.what()), std::move(warnings));
      }
      const auto assetStatus = api.instanceAssetCopy(
          implementation->engine, instance, 0u, &generatedAsset.info,
          generatedAsset.payload.data(), generatedAsset.payload.size(),
          generatedAsset.formatTag);
      if (assetStatus != ET_OK) {
        co_return fail(
            nodeError(index, bassManagement ? "Bass Management rejected the generated FIR asset" :
                                             "native DSP rejected the generated FIR asset"),
            std::move(warnings));
      }
      if (irReverb || sfzPlayer || roomEq) {
        const auto width = sfzPlayer ? selectedProcessingChannels(channelSpec, options.maxChannels) :
                                      generatedAsset.info.processing_channels;
        auto silence = std::vector<float>(width * options.maxFrames);
        const auto frameBudget = generatedAsset.warmupFrames ? static_cast<std::uint64_t>(generatedAsset.warmupFrames) + options.maxFrames :
            static_cast<std::uint64_t>(std::max(4096u, generatedAsset.info.frames)) * generatedAsset.info.channels * 16u;
        auto processed = std::uint64_t{0};
        while (api.instanceAssetState(implementation->engine, instance, 0u) == ET_ASSET_STATE_PREPARING &&
               processed < frameBudget) {
          std::fill(silence.begin(), silence.end(), 0.0F);
          if (api.instanceProcess(implementation->engine, instance, silence.data(), width, options.maxFrames,
                                  processed / static_cast<double>(options.sampleRate)) != ET_OK) break;
          processed += options.maxFrames;
          if ((processed / options.maxFrames) % 32u == 0u) co_await preparationCheckpoint(cancellation);
        }
        if (api.instanceAssetState(implementation->engine, instance, 0u) != ET_ASSET_STATE_ACTIVE ||
            api.instanceReset(implementation->engine, instance) != ET_OK)
          co_return fail(nodeError(index, std::string(node.name) + " preparation did not reach ACTIVE"), std::move(warnings));
      }
    }
    if (sfzPlayer) {
      const auto elements = definition->elements;
      const auto detection = std::ranges::find(elements, "dt", &ParameterElement::directKey);
      const auto preview = std::ranges::find(elements, "pm", &ParameterElement::directKey);
      if (detection == elements.end() || preview == elements.end())
        co_return fail(nodeError(index, "SFZ detection parameters are unavailable"), std::move(warnings));
      if (packed.floats[std::distance(elements.begin(), detection)] == 1.0F) {
        auto &sfz = implementation->sfzEventInstances.emplace_back(SfzEventInstance{
            instance, selectedProcessingChannels(channelSpec, options.maxChannels), definition->hash,
            std::move(packed.floats), static_cast<std::size_t>(std::distance(elements.begin(), preview))});
        if (primeSfzEvent(api, implementation->engine, sfz, options.sampleRate, options.maxFrames) != ET_OK)
          co_return fail(nodeError(index, "SFZ Event analysis initialization failed"), std::move(warnings));
      }
    }
    activeNodes.push_back({.instance = instance,
                           .inputBus = inputBus,
                           .outputBus = outputBus,
                           .channelSpec = channelSpec});
    entry.state = PresetEntryState::enabled;
  }

  const auto descriptor = buildDescriptor(activeNodes);
  const auto descriptorStatus = api.pipelineConfigure(
      implementation->engine, descriptor.data(), static_cast<std::uint32_t>(descriptor.size()));
  if (descriptorStatus != ET_OK) {
    co_return fail("EffeTune rejected the native pipeline descriptor", std::move(warnings));
  }
  implementation->latencyFrames =
      api.pipelineLatency(implementation->engine);
  implementation->activePluginCount = activeNodes.size();
  for (const auto &warning : warnings)
    implementation->presetEntries[warning.nodeIndex].diagnostics.push_back(warning.reason);

  co_await preparationCheckpoint(cancellation);
  try {
    const auto verification = AssetMemoryReservation(sourceSnapshots.empty() ? 0 : 128 * 1024, context.assetMemoryBytes);
    co_await verifyAssetSnapshots(sourceSnapshots, cancellation);
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const std::exception &error) { co_return fail(error.what(), std::move(warnings)); }
  auto pipeline = std::unique_ptr<DspPipeline>(new DspPipeline(std::move(implementation)));
  auto files = std::vector<std::filesystem::path>(pipeline->dependencyFiles().begin(), pipeline->dependencyFiles().end());
  co_return PipelineLoadResult{.pipeline = std::move(pipeline), .warnings = std::move(warnings), .dependencyFiles = std::move(files),
          .cachedAssetCount = cachedAssetCount, .error = {}};
}

PipelineLoadResult loadDspPipeline(const std::filesystem::path &presetPath,
                                   const PipelineBuildOptions &options,
                                   const PipelineLoadContext &context) {
  auto backend = loadDspBackend(DspBackendKind::scalar);
  if (backend.backend == nullptr) {
    return loadError("scalar DSP backend is unavailable: " +
                     backend.error);
  }
  return loadDspPipeline(presetPath, options, std::move(backend.backend), context);
}

PipelineLoadResult
loadDspPipeline(const std::filesystem::path &presetPath,
                const PipelineBuildOptions &options,
                std::shared_ptr<const DspBackend> backend,
                const PipelineLoadContext &context) {
  return runPreparation<PipelineLoadResult>([&] {
    return loadDspPipelineAsync(presetPath, options, std::move(backend), context, {});
  });
}

cardio::promise<PipelineLoadResult> loadDspPipelineAsync(
    std::filesystem::path presetPath, PipelineBuildOptions options,
    std::shared_ptr<const DspBackend> backend, PipelineLoadContext context,
    cardio::cancellation cancellation) {
  co_await preparationCheckpoint(cancellation);
  if (presetPath.extension() != ".effetune_preset")
    co_return loadError("preset path must use the exact .effetune_preset extension");
  const auto validation = validateBuildOptions(options);
  if (!validation.empty()) co_return loadError(validation);
  auto recipe = std::shared_ptr<const std::string>{};
  try {
    const auto bytes = std::move(co_await readAssetFile(presetPath, kMaximumPresetBytes, cancellation));
    if (bytes.empty()) co_return loadError("preset file must contain between 1 byte and 8 MiB");
    recipe = std::make_shared<const std::string>(reinterpret_cast<const char *>(bytes.data()), bytes.size());
  } catch (const cardio::canceled_exception &) { throw; }
  catch (const std::exception &error) {
    co_return loadError(std::string("cannot read preset file: ") + error.what());
  }
  auto loaded = std::move(co_await DspPipeline::buildFromRecipe(
      recipe, options, std::move(backend), std::move(context), cancellation));
  if (loaded.pipeline) {
    try {
      const auto latest = std::move(co_await readAssetFile(presetPath, kMaximumPresetBytes, cancellation));
      if (latest.size() != recipe->size() ||
          !std::equal(latest.begin(), latest.end(), reinterpret_cast<const std::uint8_t *>(recipe->data()))) {
        loaded.pipeline.reset();
        loaded.error = "preset changed during preparation";
      }
    } catch (const cardio::canceled_exception &) { throw; }
    catch (const std::exception &error) {
      loaded.pipeline.reset();
      loaded.error = std::string("preset changed during preparation: ") + error.what();
    }
  }
  co_return loaded;
}

PipelineLoadResult rebuildDspPipeline(const DspPipeline &source,
                                       const PipelineBuildOptions &options) {
  return rebuildDspPipeline(source, options, {});
}

PipelineLoadResult rebuildDspPipeline(const DspPipeline &source,
    const PipelineBuildOptions &options, std::shared_ptr<const DspBackend> backend) {
  return runPreparation<PipelineLoadResult>([&] {
    return rebuildDspPipelineAsync(source, options, std::move(backend), {});
  });
}

cardio::promise<PipelineLoadResult> rebuildDspPipelineAsync(const DspPipeline &source,
    PipelineBuildOptions options, std::shared_ptr<const DspBackend> backend, cardio::cancellation cancellation) {
  // This is deliberately an ordinary function: capture the recipe while the
  // caller protects the active source, then suspend before any heavy work.
  cancellation.throw_if_cancellation_requested();
  if (source.implementation_ == nullptr) return cardio::resolved(loadError("source DSP pipeline is unavailable"));
  if (source.implementation_->bypass) {
    auto created = createBypassDspPipeline(options);
    return cardio::resolved(PipelineLoadResult{.pipeline = std::move(created.pipeline),
                                              .warnings = {}, .error = std::move(created.error)});
  }
  if (!backend) backend = source.implementation_->backend;
  return DspPipeline::buildFromRecipe(source.implementation_->presetRecipe, options,
      std::move(backend), source.implementation_->loadContext, cancellation);
}

} // namespace pipetune
