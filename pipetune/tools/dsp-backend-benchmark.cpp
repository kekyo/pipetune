#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_pipeline.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pipetune_benchmark {

struct Options {
  std::uint32_t sampleRate = 48000;
  std::uint32_t channels = 2;
  std::uint32_t frames = 256;
  std::uint32_t warmupBlocks = 100;
  std::uint32_t measureBlocks = 2000;
  bool json = false;
  bool help = false;
  std::vector<std::filesystem::path> inputs;
};

struct ParseResult {
  Options options;
  std::string error;
};

struct TimedProcessResult {
  double nanoseconds;
  double checksum;
  std::string error;
};

struct PresetBenchmarkResult {
  std::filesystem::path preset;
  std::size_t activePluginCount;
  std::size_t omittedNodeCount;
  double scalarNanosecondsPerFrame;
  double simdNanosecondsPerFrame;
  double speedup;
  double scalarChecksum;
  double simdChecksum;
};

static std::string_view usage() {
  return "Usage:\n"
         "  pipetune-dsp-benchmark [OPTIONS] PRESET_OR_DIRECTORY...\n"
         "\n"
         "Options:\n"
         "  --sample-rate HZ       DSP sample rate (default: 48000).\n"
         "  --channels COUNT       Planar channels, 1 through 8 (default: 2).\n"
         "  --frames COUNT         Frames per block, at least 32 "
         "(default: 256).\n"
         "  --warmup-blocks COUNT  Untimed blocks per backend "
         "(default: 100).\n"
         "  --measure-blocks COUNT Timed blocks per backend "
         "(default: 2000).\n"
         "  --json                  Emit one machine-readable JSON report.\n"
         "  -h, --help              Show this help.\n"
         "\n"
         "Directories are searched recursively for *.effetune_preset files.\n";
}

static bool parseUnsigned(std::string_view text, std::uint32_t minimum,
                          std::uint32_t maximum, std::uint32_t &output) {
  if (text.empty()) {
    return false;
  }
  auto value = std::uint64_t{0};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() || value < minimum ||
      value > maximum) {
    return false;
  }
  output = static_cast<std::uint32_t>(value);
  return true;
}

static ParseResult parseArguments(
    std::span<const std::string_view> arguments) {
  auto result = ParseResult{};
  auto positionalOnly = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (!positionalOnly && argument == "--") {
      positionalOnly = true;
      continue;
    }
    if (!positionalOnly && (argument == "-h" || argument == "--help")) {
      result.options.help = true;
      continue;
    }
    if (!positionalOnly && argument == "--json") {
      if (result.options.json) {
        result.error = "duplicate option: --json";
        return result;
      }
      result.options.json = true;
      continue;
    }
    if (!positionalOnly && argument.starts_with('-')) {
      if (argument != "--sample-rate" && argument != "--channels" &&
          argument != "--frames" && argument != "--warmup-blocks" &&
          argument != "--measure-blocks") {
        result.error = "unknown option: " + std::string(argument);
        return result;
      }
      if (index + 1 >= arguments.size()) {
        result.error =
            "missing value for " + std::string(argument);
        return result;
      }
      const auto value = arguments[++index];
      auto valid = false;
      if (argument == "--sample-rate") {
        valid = parseUnsigned(value, 32000, 384000,
                              result.options.sampleRate);
        if (!valid) {
          result.error =
              "--sample-rate must be between 32000 and 384000";
        }
      } else if (argument == "--channels") {
        valid = parseUnsigned(value, 1, 8, result.options.channels);
        if (!valid) {
          result.error = "--channels must be between 1 and 8";
        }
      } else if (argument == "--frames") {
        valid = parseUnsigned(value, 32, 8192, result.options.frames);
        if (!valid) {
          result.error =
              "--frames must be at least 32 and no greater than 8192";
        }
      } else if (argument == "--warmup-blocks") {
        valid = parseUnsigned(value, 0, 10000000,
                              result.options.warmupBlocks);
        if (!valid) {
          result.error =
              "--warmup-blocks must be between 0 and 10000000";
        }
      } else {
        valid = parseUnsigned(value, 1, 10000000,
                              result.options.measureBlocks);
        if (!valid) {
          result.error =
              "--measure-blocks must be between 1 and 10000000";
        }
      }
      if (!valid) {
        return result;
      }
      continue;
    }
    result.options.inputs.emplace_back(argument);
  }

  if (!result.options.help && result.options.inputs.empty()) {
    result.error = "at least one preset or directory is required";
  }
  return result;
}

static std::string collectPresetPaths(
    const std::vector<std::filesystem::path> &inputs,
    std::vector<std::filesystem::path> &presets) {
  for (const auto &input : inputs) {
    auto error = std::error_code{};
    const auto status = std::filesystem::status(input, error);
    if (error) {
      return "cannot inspect " + input.string() + ": " + error.message();
    }
    if (std::filesystem::is_regular_file(status)) {
      if (input.extension() != ".effetune_preset") {
        return "preset file must use the exact .effetune_preset extension: " +
               input.string();
      }
      presets.push_back(input.lexically_normal());
      continue;
    }
    if (!std::filesystem::is_directory(status)) {
      return "benchmark input is not a preset or directory: " +
             input.string();
    }

    auto iterator = std::filesystem::recursive_directory_iterator(
        input, std::filesystem::directory_options::skip_permission_denied,
        error);
    const auto end = std::filesystem::recursive_directory_iterator{};
    while (!error && iterator != end) {
      const auto &entry = *iterator;
      if (entry.is_regular_file(error) && !error &&
          entry.path().extension() == ".effetune_preset") {
        presets.push_back(entry.path().lexically_normal());
      }
      iterator.increment(error);
    }
    if (error) {
      return "cannot enumerate " + input.string() + ": " + error.message();
    }
  }

  std::sort(presets.begin(), presets.end());
  presets.erase(std::unique(presets.begin(), presets.end()),
                presets.end());
  return presets.empty() ? std::string("no EffeTune presets were found")
                         : std::string{};
}

static std::vector<float> makeInput(std::uint32_t channels,
                                    std::uint32_t frames) {
  auto samples = std::vector<float>(
      static_cast<std::size_t>(channels) * frames);
  auto state = std::uint32_t{0x6d2b79f5u};
  for (auto &sample : samples) {
    state = state * 1664525u + 1013904223u;
    const auto centered =
        static_cast<std::int32_t>(state >> 8u) - 0x7fffff;
    sample = static_cast<float>(centered) / 16777216.0F * 0.25F;
  }
  return samples;
}

static std::string processBlock(
    pipetune::DspPipeline &pipeline, std::span<const float> input,
    std::vector<float> &working, std::uint32_t channels,
    std::uint32_t frames, double timeSeconds) {
  std::copy(input.begin(), input.end(), working.begin());
  const auto status =
      pipeline.process(working, channels, frames, timeSeconds);
  if (status == pipetune::ProcessStatus::ok) {
    return {};
  }
  return status == pipetune::ProcessStatus::invalidBuffer
             ? std::string("DSP pipeline rejected the benchmark buffer")
             : std::string("EffeTune rejected a benchmark process call");
}

static TimedProcessResult processTimedBlock(
    pipetune::DspPipeline &pipeline, std::span<const float> input,
    std::vector<float> &working, std::uint32_t channels,
    std::uint32_t frames, double timeSeconds,
    std::size_t checksumIndex) {
  std::copy(input.begin(), input.end(), working.begin());
  const auto started = std::chrono::steady_clock::now();
  const auto status =
      pipeline.process(working, channels, frames, timeSeconds);
  const auto stopped = std::chrono::steady_clock::now();
  if (status != pipetune::ProcessStatus::ok) {
    return {
        .nanoseconds = 0.0,
        .checksum = 0.0,
        .error = status == pipetune::ProcessStatus::invalidBuffer
                     ? std::string(
                           "DSP pipeline rejected the benchmark buffer")
                     : std::string(
                           "EffeTune rejected a benchmark process call"),
    };
  }
  return {
      .nanoseconds =
          std::chrono::duration<double, std::nano>(stopped - started)
              .count(),
      .checksum =
          static_cast<double>(working[checksumIndex % working.size()]),
      .error = {},
  };
}

static std::string benchmarkPreset(
    const std::filesystem::path &preset, const Options &options,
    const pipetune::DspBackends &backends,
    PresetBenchmarkResult &output) {
  const auto buildOptions = pipetune::PipelineBuildOptions{
      .sampleRate = static_cast<float>(options.sampleRate),
      .maxChannels = options.channels,
      .maxFrames = options.frames,
  };
  auto scalar = pipetune::loadDspPipeline(
      preset, buildOptions, backends.scalar.backend);
  if (scalar.pipeline == nullptr) {
    return "cannot build scalar pipeline for " + preset.string() + ": " +
           scalar.error;
  }
  auto simd = pipetune::loadDspPipeline(
      preset, buildOptions, backends.simd.backend);
  if (simd.pipeline == nullptr) {
    return "cannot build SIMD pipeline for " + preset.string() + ": " +
           simd.error;
  }
  if (scalar.pipeline->activePluginCount() !=
      simd.pipeline->activePluginCount()) {
    return "backend plugin counts differ for " + preset.string();
  }

  const auto input = makeInput(options.channels, options.frames);
  auto scalarWorking = input;
  auto simdWorking = input;
  for (auto block = std::uint32_t{0}; block < options.warmupBlocks;
       ++block) {
    const auto timeSeconds =
        static_cast<double>(block) * options.frames /
        static_cast<double>(options.sampleRate);
    auto error = processBlock(*scalar.pipeline, input, scalarWorking,
                              options.channels, options.frames, timeSeconds);
    if (!error.empty()) {
      return "scalar warmup failed for " + preset.string() + ": " + error;
    }
    error = processBlock(*simd.pipeline, input, simdWorking,
                         options.channels, options.frames, timeSeconds);
    if (!error.empty()) {
      return "SIMD warmup failed for " + preset.string() + ": " + error;
    }
  }

  auto scalarNanoseconds = 0.0;
  auto simdNanoseconds = 0.0;
  auto scalarChecksum = 0.0;
  auto simdChecksum = 0.0;
  for (auto block = std::uint32_t{0}; block < options.measureBlocks;
       ++block) {
    const auto sequence = options.warmupBlocks + block;
    const auto timeSeconds =
        static_cast<double>(sequence) * options.frames /
        static_cast<double>(options.sampleRate);
    auto scalarResult = TimedProcessResult{};
    auto simdResult = TimedProcessResult{};
    if ((block & 1u) == 0u) {
      scalarResult = processTimedBlock(
          *scalar.pipeline, input, scalarWorking, options.channels,
          options.frames, timeSeconds, block);
      simdResult = processTimedBlock(
          *simd.pipeline, input, simdWorking, options.channels,
          options.frames, timeSeconds, block);
    } else {
      simdResult = processTimedBlock(
          *simd.pipeline, input, simdWorking, options.channels,
          options.frames, timeSeconds, block);
      scalarResult = processTimedBlock(
          *scalar.pipeline, input, scalarWorking, options.channels,
          options.frames, timeSeconds, block);
    }
    if (!scalarResult.error.empty()) {
      return "scalar measurement failed for " + preset.string() + ": " +
             scalarResult.error;
    }
    if (!simdResult.error.empty()) {
      return "SIMD measurement failed for " + preset.string() + ": " +
             simdResult.error;
    }
    scalarNanoseconds += scalarResult.nanoseconds;
    simdNanoseconds += simdResult.nanoseconds;
    scalarChecksum += scalarResult.checksum;
    simdChecksum += simdResult.checksum;
  }

  const auto measuredFrames =
      static_cast<double>(options.measureBlocks) * options.frames;
  const auto scalarPerFrame = scalarNanoseconds / measuredFrames;
  const auto simdPerFrame = simdNanoseconds / measuredFrames;
  if (!std::isfinite(scalarPerFrame) || scalarPerFrame <= 0.0 ||
      !std::isfinite(simdPerFrame) || simdPerFrame <= 0.0) {
    return "benchmark clock did not produce a usable duration for " +
           preset.string();
  }
  output = {
      .preset = preset,
      .activePluginCount = scalar.pipeline->activePluginCount(),
      .omittedNodeCount = scalar.warnings.size(),
      .scalarNanosecondsPerFrame = scalarPerFrame,
      .simdNanosecondsPerFrame = simdPerFrame,
      .speedup = scalarPerFrame / simdPerFrame,
      .scalarChecksum = scalarChecksum,
      .simdChecksum = simdChecksum,
  };
  return {};
}

static std::string jsonEscape(std::string_view value) {
  constexpr auto hexadecimal = std::string_view("0123456789abcdef");
  auto escaped = std::string{};
  escaped.reserve(value.size() + 2);
  for (const auto raw : value) {
    const auto character = static_cast<unsigned char>(raw);
    switch (character) {
    case '"':
      escaped += "\\\"";
      break;
    case '\\':
      escaped += "\\\\";
      break;
    case '\b':
      escaped += "\\b";
      break;
    case '\f':
      escaped += "\\f";
      break;
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      if (character < 0x20u) {
        escaped += "\\u00";
        escaped.push_back(hexadecimal[(character >> 4u) & 0x0fu]);
        escaped.push_back(hexadecimal[character & 0x0fu]);
      } else {
        escaped.push_back(static_cast<char>(character));
      }
      break;
    }
  }
  return escaped;
}

static void printJson(
    const Options &options, const pipetune::DspBackends &backends,
    const std::vector<PresetBenchmarkResult> &results) {
  std::cout << std::setprecision(12)
            << "{\"sampleRate\":" << options.sampleRate
            << ",\"channels\":" << options.channels
            << ",\"framesPerBlock\":" << options.frames
            << ",\"warmupBlocks\":" << options.warmupBlocks
            << ",\"measureBlocks\":" << options.measureBlocks
            << ",\"scalarCpuRequirement\":\""
            << jsonEscape(backends.scalar.cpuRequirement)
            << "\",\"simdCpuRequirement\":\""
            << jsonEscape(backends.simd.cpuRequirement)
            << "\",\"results\":[";
  for (auto index = std::size_t{0}; index < results.size(); ++index) {
    const auto &result = results[index];
    if (index != 0) {
      std::cout << ',';
    }
    std::cout << "{\"preset\":\""
              << jsonEscape(result.preset.string())
              << "\",\"activePluginCount\":"
              << result.activePluginCount
              << ",\"omittedNodeCount\":" << result.omittedNodeCount
              << ",\"scalarNanosecondsPerFrame\":"
              << result.scalarNanosecondsPerFrame
              << ",\"simdNanosecondsPerFrame\":"
              << result.simdNanosecondsPerFrame
              << ",\"speedup\":" << result.speedup
              << ",\"scalarChecksum\":" << result.scalarChecksum
              << ",\"simdChecksum\":" << result.simdChecksum << '}';
  }
  std::cout << "]}\n";
}

static void printTable(
    const Options &options, const pipetune::DspBackends &backends,
    const std::vector<PresetBenchmarkResult> &results) {
  std::cout << "DSP backend benchmark: " << options.sampleRate << " Hz, "
            << options.channels << " channels, " << options.frames
            << " frames/block, " << options.measureBlocks
            << " measured blocks\n"
            << "Scalar CPU requirement: "
            << backends.scalar.cpuRequirement << '\n'
            << "SIMD CPU requirement: " << backends.simd.cpuRequirement
            << "\n\n"
            << std::left << std::setw(42) << "Preset" << std::right
            << std::setw(8) << "Nodes" << std::setw(16)
            << "Scalar ns/frame" << std::setw(16) << "SIMD ns/frame"
            << std::setw(12) << "Speedup" << '\n';
  for (const auto &result : results) {
    auto name = result.preset.filename().string();
    if (name.size() > 40) {
      name.resize(40);
    }
    std::cout << std::left << std::setw(42) << name << std::right
              << std::setw(8) << result.activePluginCount
              << std::setw(16) << std::fixed << std::setprecision(3)
              << result.scalarNanosecondsPerFrame << std::setw(16)
              << result.simdNanosecondsPerFrame << std::setw(11)
              << result.speedup << "x\n";
    if (result.omittedNodeCount != 0) {
      std::cout << "  omitted unsupported/asset-backed nodes: "
                << result.omittedNodeCount << '\n';
    }
  }
}

static int run(std::span<const std::string_view> arguments) {
  const auto parsed = parseArguments(arguments);
  if (!parsed.error.empty()) {
    std::cerr << "pipetune-dsp-benchmark: " << parsed.error << "\n\n"
              << usage();
    return 2;
  }
  if (parsed.options.help) {
    std::cout << usage();
    return 0;
  }

  auto presets = std::vector<std::filesystem::path>{};
  const auto collectionError =
      collectPresetPaths(parsed.options.inputs, presets);
  if (!collectionError.empty()) {
    std::cerr << "pipetune-dsp-benchmark: " << collectionError << '\n';
    return 1;
  }

  const auto backends = pipetune::discoverDspBackends();
  if (backends.scalar.backend == nullptr) {
    std::cerr << "pipetune-dsp-benchmark: scalar DSP backend is "
                 "unavailable: "
              << backends.scalar.error << '\n';
    return 1;
  }
  if (backends.simd.backend == nullptr) {
    std::cerr << "pipetune-dsp-benchmark: SIMD DSP backend is unavailable: "
              << backends.simd.error << '\n';
    return 1;
  }

  auto results = std::vector<PresetBenchmarkResult>{};
  results.reserve(presets.size());
  for (const auto &preset : presets) {
    auto result = PresetBenchmarkResult{};
    const auto error =
        benchmarkPreset(preset, parsed.options, backends, result);
    if (!error.empty()) {
      std::cerr << "pipetune-dsp-benchmark: " << error << '\n';
      return 1;
    }
    results.push_back(std::move(result));
  }

  if (parsed.options.json) {
    printJson(parsed.options, backends, results);
  } else {
    printTable(parsed.options, backends, results);
  }
  return 0;
}

} // namespace pipetune_benchmark

int main(int argc, char **argv) {
  auto arguments = std::vector<std::string_view>{};
  arguments.reserve(argc > 1 ? static_cast<std::size_t>(argc - 1) : 0);
  for (auto index = 1; index < argc; ++index) {
    arguments.emplace_back(argv[index]);
  }
  return pipetune_benchmark::run(arguments);
}
