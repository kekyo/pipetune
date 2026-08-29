/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <effetune/abi.h>

#include "effetune_backend_abi.h"

#include <dlfcn.h>

#if defined(__aarch64__)
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

static int failures = 0;

static_assert(EFFETUNE_DSP_ABI_VERSION == 1u);

static void check(bool condition, const char *message) {
  if (!condition) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s\n", message);
    ++failures;
  }
}

template <typename Function>
static Function loadFunction(void *handle, const char *name) {
  dlerror();
  void *address = dlsym(handle, name);
  const char *error = dlerror();
  if (error != nullptr || address == nullptr) {
    std::fprintf(stderr, "Cannot load %s: %s\n", name,
                 error == nullptr ? "symbol is null" : error);
    ++failures;
    return nullptr;
  }
  static_assert(sizeof(Function) == sizeof(address));
  auto function = Function{};
  std::memcpy(&function, &address, sizeof(function));
  return function;
}

struct BackendApi {
  void *handle = nullptr;
  std::uint32_t (*abiVersion)() = nullptr;
  std::uint32_t (*buildFlags)() = nullptr;
  std::uint32_t (*backendVariant)() = nullptr;
  std::uint32_t (*kernelCount)() = nullptr;
  std::int32_t (*kernelName)(std::uint32_t, char *, std::uint32_t) = nullptr;
  std::uint32_t (*kernelParamsHash)(std::uint32_t) = nullptr;
  std::uint32_t (*kernelParamBytesCapacity)(std::uint32_t) = nullptr;
  std::uint32_t (*kernelAssetCapacity)(std::uint32_t, std::uint32_t) = nullptr;
  et_design_fft *(*designFftCreate)(std::uint32_t) = nullptr;
  void (*designFftDestroy)(et_design_fft *) = nullptr;
  float *(*designFftInput)(et_design_fft *) = nullptr;
  const float *(*designFftOutput)(const et_design_fft *) = nullptr;
  et_status (*designFftForward)(et_design_fft *) = nullptr;
  et_status (*designFftInverse)(et_design_fft *) = nullptr;
  et_engine (*engineCreate)() = nullptr;
  void (*engineDestroy)(et_engine) = nullptr;
  et_status (*enginePrepare)(et_engine, float, std::uint32_t, std::uint32_t,
                             std::uint32_t) = nullptr;
  et_instance (*instanceCreate)(et_engine, const char *) = nullptr;
  void (*instanceDestroy)(et_engine, et_instance) = nullptr;
  std::uint32_t (*instanceLatency)(et_engine, et_instance) = nullptr;
  et_status (*instanceSetSeed)(et_engine, et_instance, std::uint32_t,
                               std::uint32_t) = nullptr;
  et_status (*instanceSetParams)(et_engine, et_instance, const float *,
                                 std::uint32_t, std::uint32_t,
                                 std::uint32_t) = nullptr;
  et_status (*instanceProcess)(et_engine, et_instance, float *, std::uint32_t,
                               std::uint32_t, double) = nullptr;
  et_status (*instanceRuntimeEvent)(et_engine, et_instance,
                                    et_runtime_event_state *) = nullptr;
};

static BackendApi loadBackend(const std::filesystem::path &path) {
  auto api = BackendApi{};
  api.handle = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (api.handle == nullptr) {
    const char *error = dlerror();
    std::fprintf(stderr, "Cannot load %s: %s\n", path.c_str(),
                 error == nullptr ? "unknown dynamic loader error" : error);
    ++failures;
    return api;
  }
  api.abiVersion =
      loadFunction<decltype(api.abiVersion)>(api.handle, "et_abi_version");
  api.buildFlags =
      loadFunction<decltype(api.buildFlags)>(api.handle, "et_build_flags");
  api.backendVariant = loadFunction<decltype(api.backendVariant)>(
      api.handle, "pipetune_effetune_backend_variant");
  api.kernelCount =
      loadFunction<decltype(api.kernelCount)>(api.handle, "et_kernel_count");
  api.kernelName =
      loadFunction<decltype(api.kernelName)>(api.handle, "et_kernel_name");
  api.kernelParamsHash = loadFunction<decltype(api.kernelParamsHash)>(
      api.handle, "et_kernel_params_hash");
  api.kernelParamBytesCapacity =
      loadFunction<decltype(api.kernelParamBytesCapacity)>(
          api.handle, "et_kernel_param_bytes_capacity");
  api.kernelAssetCapacity =
      loadFunction<decltype(api.kernelAssetCapacity)>(
          api.handle, "et_kernel_asset_capacity");
  api.designFftCreate = loadFunction<decltype(api.designFftCreate)>(
      api.handle, "et_design_fft_create");
  api.designFftDestroy = loadFunction<decltype(api.designFftDestroy)>(
      api.handle, "et_design_fft_destroy");
  api.designFftInput = loadFunction<decltype(api.designFftInput)>(
      api.handle, "et_design_fft_input");
  api.designFftOutput = loadFunction<decltype(api.designFftOutput)>(
      api.handle, "et_design_fft_output");
  api.designFftForward = loadFunction<decltype(api.designFftForward)>(
      api.handle, "et_design_fft_forward");
  api.designFftInverse = loadFunction<decltype(api.designFftInverse)>(
      api.handle, "et_design_fft_inverse");
  api.engineCreate =
      loadFunction<decltype(api.engineCreate)>(api.handle, "et_engine_create");
  api.engineDestroy = loadFunction<decltype(api.engineDestroy)>(
      api.handle, "et_engine_destroy");
  api.enginePrepare = loadFunction<decltype(api.enginePrepare)>(
      api.handle, "et_engine_prepare");
  api.instanceCreate = loadFunction<decltype(api.instanceCreate)>(
      api.handle, "et_instance_create");
  api.instanceDestroy = loadFunction<decltype(api.instanceDestroy)>(
      api.handle, "et_instance_destroy");
  api.instanceLatency = loadFunction<decltype(api.instanceLatency)>(
      api.handle, "et_instance_latency");
  api.instanceSetSeed = loadFunction<decltype(api.instanceSetSeed)>(
      api.handle, "et_instance_set_seed");
  api.instanceSetParams = loadFunction<decltype(api.instanceSetParams)>(
      api.handle, "et_instance_set_params");
  api.instanceProcess = loadFunction<decltype(api.instanceProcess)>(
      api.handle, "et_instance_process");
  api.instanceRuntimeEvent = loadFunction<decltype(api.instanceRuntimeEvent)>(
      api.handle, "et_instance_runtime_event");
  return api;
}

static void closeBackend(BackendApi &api) {
  if (api.handle != nullptr) {
    check(dlclose(api.handle) == 0, "backend must close successfully");
    api.handle = nullptr;
  }
}

static std::string kernelName(const BackendApi &api, std::uint32_t index) {
  const auto length = api.kernelName(index, nullptr, 0);
  if (length < 0) {
    return {};
  }
  auto name = std::string(static_cast<std::size_t>(length) + 1u, '\0');
  const auto copied =
      api.kernelName(index, name.data(), static_cast<std::uint32_t>(name.size()));
  if (copied != length) {
    return {};
  }
  name.resize(static_cast<std::size_t>(length));
  return name;
}

static std::vector<float> renderImpulseSpectrum(const BackendApi &api) {
  constexpr auto fftSize = std::uint32_t{256};
  et_design_fft *fft = api.designFftCreate(fftSize);
  check(fft != nullptr, "design FFT must be constructible");
  if (fft == nullptr) {
    return {};
  }
  float *input = api.designFftInput(fft);
  check(input != nullptr, "design FFT input must be available");
  if (input == nullptr) {
    api.designFftDestroy(fft);
    return {};
  }
  std::fill_n(input, fftSize, 0.0F);
  input[0] = 1.0F;
  check(api.designFftForward(fft) == ET_OK,
        "design FFT forward transform must succeed");
  const float *output = api.designFftOutput(fft);
  check(output != nullptr, "design FFT output must be available");
  auto spectrum = output == nullptr
                      ? std::vector<float>{}
                      : std::vector<float>(output, output + fftSize);
  check(std::ranges::all_of(spectrum, [](float value) {
          return std::isfinite(value);
        }),
        "design FFT output must be finite");
  api.designFftDestroy(fft);
  return spectrum;
}

static void checkCatalogsMatch(const BackendApi &scalar,
                               const BackendApi &simd) {
  const auto scalarCount = scalar.kernelCount();
  const auto simdCount = simd.kernelCount();
  check(scalarCount != 0u, "production backend catalog must not be empty");
  check(scalarCount == simdCount,
        "scalar and SIMD backend catalogs must have equal counts");
  for (auto index = std::uint32_t{0}; index < scalarCount && index < simdCount;
       ++index) {
    const auto scalarName = kernelName(scalar, index);
    check(!scalarName.empty(), "kernel name must be available");
    check(scalarName == kernelName(simd, index),
          "kernel names must match between variants");
    check(scalar.kernelParamsHash(index) == simd.kernelParamsHash(index),
          "kernel parameter hashes must match between variants");
    check(scalar.kernelParamBytesCapacity(index) ==
              simd.kernelParamBytesCapacity(index),
          "kernel parameter byte capacities must match between variants");
    for (auto slot = std::uint32_t{0}; slot < 4u; ++slot) {
      check(scalar.kernelAssetCapacity(index, slot) ==
                simd.kernelAssetCapacity(index, slot),
            "kernel asset capacities must match between variants");
    }
  }
}

static std::uint32_t findKernelIndex(const BackendApi &api,
                                     std::string_view typeName) {
  const auto count = api.kernelCount();
  for (auto index = std::uint32_t{0}; index < count; ++index) {
    if (kernelName(api, index) == typeName) {
      return index;
    }
  }
  return count;
}

static void checkEffeTune26Catalog(const BackendApi &api) {
  static constexpr std::array<std::string_view, 2> addedTypes = {
      "GroupDelayPEQPlugin", "MDSimulatorPlugin"};
  check(api.kernelCount() == 94u,
        "EffeTune 2.6 backend catalog must contain 94 kernels");
  for (const auto typeName : addedTypes) {
    check(findKernelIndex(api, typeName) < api.kernelCount(),
          "EffeTune 2.6 backend catalog must contain every new kernel");
  }
  const auto phaseSelectIndex = findKernelIndex(api, "PhaseSelectEqPlugin");
  check(phaseSelectIndex < api.kernelCount(),
        "EffeTune 2.6 backend catalog must retain Phase Select EQ");
  if (phaseSelectIndex < api.kernelCount()) {
    check(api.kernelParamsHash(phaseSelectIndex) == 0x51c6d77au,
          "EffeTune 2.6 Phase Select EQ must use its expanded parameter layout");
  }
  const auto tubeIndex = findKernelIndex(api, "TubeSimulatorPlugin");
  check(tubeIndex < api.kernelCount(),
        "EffeTune 2.6 backend catalog must retain Tube Simulator");
  if (tubeIndex < api.kernelCount()) {
    check(api.kernelParamsHash(tubeIndex) == 0x07986b4bu,
          "EffeTune 2.6 Tube Simulator must retain its parameter layout");
  }
}

static void checkTubeRuntimeContract(const BackendApi &api) {
  constexpr auto telemetryBytes = std::uint32_t{64u * 1024u};
  const auto engine = api.engineCreate();
  check(engine != 0u, "Tube Simulator test engine must be created");
  if (engine == 0u) {
    return;
  }

  auto instance = et_instance{0};
  const auto prepared =
      api.enginePrepare(engine, 48000.0F, 2u, 64u, telemetryBytes);
  check(prepared == ET_OK, "Tube Simulator test engine must be prepared");
  if (prepared == ET_OK) {
    instance = api.instanceCreate(engine, "TubeSimulatorPlugin");
    check(instance != 0u, "Tube Simulator instance must be created");
  }
  if (instance != 0u) {
    check(api.instanceLatency(engine, instance) == 64u,
          "Tube Simulator must expose its 64-frame latency");
    auto event = et_runtime_event_state{};
    check(api.instanceRuntimeEvent(engine, instance, &event) == ET_OK,
          "Tube Simulator runtime event must be readable");
    check(event.generation == 0u && event.latched == 0u && event.cause == 0u,
          "Tube Simulator must start without a latched runtime event");

    auto faultInput = std::array{
        std::numeric_limits<float>::quiet_NaN(), 0.0F};
    check(api.instanceProcess(engine, instance, faultInput.data(), 2u, 1u,
                              0.0) == ET_OK,
          "Tube Simulator must recover from non-finite input");
    check(std::ranges::all_of(faultInput, [](float value) {
            return std::isfinite(value);
          }),
          "Tube Simulator safety recovery must emit finite audio");
    check(api.instanceRuntimeEvent(engine, instance, &event) == ET_OK,
          "Tube Simulator safety runtime event must be readable");
    check(event.generation == 1u && event.latched == 1u && event.cause == 2u,
          "Tube Simulator must publish its processing-safety runtime event");
    api.instanceDestroy(engine, instance);
  }
  api.engineDestroy(engine);
}

struct GoldenCase {
  const char *name;
  const char *typeName;
  float sampleRate;
  std::uint32_t frameCount;
  std::uint32_t channelCount;
  std::uint32_t blockSize;
  std::uint64_t seed;
  std::span<const float> parameters;
  float tolerance;
  std::vector<float> expected;
};

static std::vector<float> readGoldenAudio(const std::filesystem::path &path,
                                          const char *name) {
  auto input = std::ifstream(path, std::ios::binary | std::ios::ate);
  if (!input) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s golden "
                         "output must be readable\n",
                 name);
    ++failures;
    return {};
  }
  const auto byteCount = input.tellg();
  if (byteCount <= 0 || byteCount % static_cast<std::streamoff>(sizeof(float)) !=
                            std::streamoff{0}) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s golden "
                         "output must contain float32 samples\n",
                 name);
    ++failures;
    return {};
  }
  auto audio = std::vector<float>(
      static_cast<std::size_t>(byteCount) / sizeof(float));
  input.seekg(0, std::ios::beg);
  input.read(reinterpret_cast<char *>(audio.data()),
             static_cast<std::streamsize>(byteCount));
  if (!input) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s golden "
                         "output must be read completely\n",
                 name);
    ++failures;
    return {};
  }
  return audio;
}

static std::vector<float> seededNoiseInput(std::uint32_t frameCount,
                                           std::uint32_t channelCount,
                                           std::uint64_t seed) {
  constexpr auto float53Denominator = 9007199254740992.0;
  auto state = seed;
  auto input = std::vector<float>(
      static_cast<std::size_t>(frameCount) * channelCount);
  for (auto &sample : input) {
    auto value = state;
    value ^= value << 13u;
    value ^= value >> 7u;
    value ^= value << 17u;
    state = value;
    const auto uniform =
        static_cast<double>(value >> 11u) / float53Denominator;
    sample = static_cast<float>(uniform * 2.0 - 1.0);
  }
  return input;
}

static std::vector<float> renderGoldenCase(const BackendApi &api,
                                           const GoldenCase &testCase) {
  constexpr auto telemetryBytes = std::uint32_t{64u * 1024u};
  const auto kernelIndex = findKernelIndex(api, testCase.typeName);
  if (kernelIndex >= api.kernelCount()) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s kernel "
                         "must be available\n",
                 testCase.name);
    ++failures;
    return {};
  }

  const auto engine = api.engineCreate();
  if (engine == 0u) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s test "
                         "engine must be created\n",
                 testCase.name);
    ++failures;
    return {};
  }
  auto audio = std::vector<float>{};
  auto instance = et_instance{0};
  const auto prepared = api.enginePrepare(
      engine, testCase.sampleRate, testCase.channelCount, testCase.blockSize,
      telemetryBytes);
  if (prepared != ET_OK) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s test "
                         "engine must be prepared\n",
                 testCase.name);
    ++failures;
  }
  if (prepared == ET_OK) {
    instance = api.instanceCreate(engine, testCase.typeName);
    if (instance == 0u) {
      std::fprintf(stderr, "EffeTune backend artifact check failed: %s "
                           "instance must be created\n",
                   testCase.name);
      ++failures;
    }
  }
  if (instance != 0u) {
    const auto seeded = api.instanceSetSeed(
        engine, instance, static_cast<std::uint32_t>(testCase.seed),
        static_cast<std::uint32_t>(testCase.seed >> 32u));
    const auto staged = api.instanceSetParams(
        engine, instance, testCase.parameters.data(),
        static_cast<std::uint32_t>(testCase.parameters.size()),
        api.kernelParamsHash(kernelIndex), 0u);
    if (seeded != ET_OK || staged != ET_OK) {
      std::fprintf(stderr, "EffeTune backend artifact check failed: %s "
                           "instance must accept its seed and parameters\n",
                   testCase.name);
      ++failures;
    }
    if (seeded == ET_OK && staged == ET_OK) {
      const auto input = seededNoiseInput(
          testCase.frameCount, testCase.channelCount, testCase.seed);
      audio = input;
      auto block = std::vector<float>(
          static_cast<std::size_t>(testCase.channelCount) *
          testCase.blockSize);
      for (auto startFrame = std::uint32_t{0};
           startFrame < testCase.frameCount;) {
        const auto blockFrames =
            std::min(testCase.blockSize, testCase.frameCount - startFrame);
        for (auto channel = std::uint32_t{0};
             channel < testCase.channelCount;
             ++channel) {
          const auto sourceOffset =
              static_cast<std::size_t>(channel) * testCase.frameCount +
              startFrame;
          const auto blockOffset =
              static_cast<std::size_t>(channel) * blockFrames;
          std::copy_n(input.data() + sourceOffset, blockFrames,
                      block.data() + blockOffset);
        }
        const auto processed = api.instanceProcess(
            engine, instance, block.data(), testCase.channelCount, blockFrames,
            static_cast<double>(startFrame) / testCase.sampleRate);
        if (processed != ET_OK) {
          std::fprintf(stderr, "EffeTune backend artifact check failed: %s "
                               "instance must process every block\n",
                       testCase.name);
          ++failures;
          audio.clear();
          break;
        }
        for (auto channel = std::uint32_t{0};
             channel < testCase.channelCount;
             ++channel) {
          const auto targetOffset =
              static_cast<std::size_t>(channel) * testCase.frameCount +
              startFrame;
          const auto blockOffset =
              static_cast<std::size_t>(channel) * blockFrames;
          std::copy_n(block.data() + blockOffset, blockFrames,
                      audio.data() + targetOffset);
        }
        startFrame += blockFrames;
      }
    }
    api.instanceDestroy(engine, instance);
  }
  api.engineDestroy(engine);
  if (!std::ranges::all_of(audio,
                           [](float value) { return std::isfinite(value); })) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s output "
                         "must remain finite\n",
                 testCase.name);
    ++failures;
  }
  return audio;
}

static void checkGoldenCase(const BackendApi &api, std::uint32_t variant,
                            const GoldenCase &testCase) {
  const auto actual = renderGoldenCase(api, testCase);
  if (actual.size() != testCase.expected.size()) {
    std::fprintf(stderr, "EffeTune backend artifact check failed: %s output "
                         "size must match the official golden\n",
                 testCase.name);
    ++failures;
    return;
  }
  auto maximumDifference = 0.0F;
  auto maximumIndex = std::size_t{0};
  for (auto index = std::size_t{0}; index < actual.size(); ++index) {
    const auto difference = std::abs(actual[index] - testCase.expected[index]);
    if (difference > maximumDifference) {
      maximumDifference = difference;
      maximumIndex = index;
    }
  }
  if (maximumDifference > testCase.tolerance) {
    std::fprintf(stderr,
                 "EffeTune backend variant %u differs from the official "
                 "%s golden at sample %zu: %.9g > %.9g\n",
                 variant, testCase.name, maximumIndex,
                 static_cast<double>(maximumDifference),
                 static_cast<double>(testCase.tolerance));
    ++failures;
  }
}

static void checkGoldenCases(const BackendApi &api, std::uint32_t variant,
                             std::span<const GoldenCase> testCases) {
  for (const auto &testCase : testCases) {
    checkGoldenCase(api, variant, testCase);
  }
}

static void checkAllAbiSymbols(void *handle) {
  static constexpr std::array names = {
      "et_abi_version",
      "et_build_flags",
      "pipetune_effetune_backend_variant",
      "et_kernel_count",
      "et_kernel_name",
      "et_kernel_params_hash",
      "et_kernel_param_bytes_capacity",
      "et_kernel_asset_capacity",
      "et_design_fft_create",
      "et_design_fft_destroy",
      "et_design_fft_input",
      "et_design_fft_output",
      "et_design_fft_forward",
      "et_design_fft_inverse",
      "et_engine_memory_required",
      "et_engine_create",
      "et_engine_destroy",
      "et_engine_prepare",
      "et_engine_reset",
      "et_engine_set_telemetry_rate",
      "et_instance_create",
      "et_instance_destroy",
      "et_instance_reset",
      "et_instance_latency",
      "et_instance_set_tap",
      "et_instance_set_seed",
      "et_instance_set_params",
      "et_instance_set_param_bytes",
      "et_instance_asset_begin",
      "et_instance_asset_commit",
      "et_instance_asset_abort",
      "et_instance_asset_state",
      "et_instance_process",
      "et_instance_runtime_event",
      "et_arena_combined_ptr",
      "et_arena_bus_ptr",
      "et_arena_scratch_ptr",
      "et_scratch_ptr",
      "et_telemetry_staging_ptr",
      "et_telemetry_capacity",
      "et_telemetry_read",
      "et_pipeline_configure",
      "et_pipeline_latency",
      "et_pipeline_process"};
  for (const char *name : names) {
    dlerror();
    void *address = dlsym(handle, name);
    check(dlerror() == nullptr && address != nullptr,
          "every required backend ABI symbol must be exported");
  }

  dlerror();
  void *legacyVariant = dlsym(handle, "et_backend_variant");
  const char *legacyVariantError = dlerror();
  check(legacyVariantError != nullptr && legacyVariant == nullptr,
        "the backend must not extend the official EffeTune ABI");
}

static std::uint32_t expectedVariant(const std::filesystem::path &path) {
  const auto filename = path.filename().string();
  if (filename == "libeffetune-dsp-scalar.so") {
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR;
  }
  if (filename == "libeffetune-dsp-simd.so") {
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_SIMD_BASELINE;
  }
  if (filename == "libeffetune-dsp-simd-x86-64-v3.so") {
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V3;
  }
  if (filename == "libeffetune-dsp-simd-x86-64-v4.so") {
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V4;
  }
  if (filename == "libeffetune-dsp-simd-arm64-sve.so") {
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_ARM64_SVE;
  }
  check(false, "backend filename must identify a supported variant");
  return PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR;
}

static bool cpuSupports(std::uint32_t variant) {
  if (variant == PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR ||
      variant == PIPETUNE_EFFETUNE_BACKEND_VARIANT_SIMD_BASELINE) {
    return true;
  }
  if (variant == PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V3) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("x86-64-v3") != 0;
#else
    return false;
#endif
  }
  if (variant == PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V4) {
#if defined(__x86_64__)
    __builtin_cpu_init();
    return __builtin_cpu_supports("x86-64-v4") != 0;
#else
    return false;
#endif
  }
  if (variant == PIPETUNE_EFFETUNE_BACKEND_VARIANT_ARM64_SVE) {
#if defined(__aarch64__)
    return (getauxval(AT_HWCAP) & HWCAP_SVE) != 0u;
#else
    return false;
#endif
  }
  return false;
}

int main(int argc, char **argv) {
  if (argc < 8) {
    std::fprintf(stderr,
                 "usage: effetune_backend_artifact_test "
                 "AUTO_LEVELER_GOLDEN_F32 BLUETOOTH_SBC_GOLDEN_F32 "
                 "CASSETTE_GOLDEN_F32 "
                 "TAPE_GOLDEN_F32 VINYL_GOLDEN_F32 "
                 "SCALAR_SO SIMD_SO [ISA_SIMD_SO...]\n");
    return 2;
  }

  static constexpr std::array autoLevelerParameters = {
      -20.0F, 1000.0F, 12.0F, -36.0F, 1.0F, 10.0F, -96.0F};
  static constexpr std::array bluetoothSbcParameters = {
      35.0F, 0.0F, 3.0F, 0.0F, 100.0F, 0.0F};
  static constexpr std::array cassetteParameters = {
      2.0F, 0.0F, 1.0F, 0.0F, 9.0F, 0.25F,
      -60.5F, 2.0F, 2.0F, 0.0F, 0.0F, 100.0F};
  static constexpr std::array tapeParameters = {
      1.0F, 0.0F, 0.0F, 6.0F, 0.15625F, -62.5F, 0.0F, 100.0F};
  static constexpr std::array vinylParameters = {
      120.0F, 0.0F, 2000.0F, 0.0F, 0.0F, 0.0F,
      100.0F, 0.0F, 200.0F, 100.0F, 0.0F, 100.0F};
  auto goldenCases = std::array{
      GoldenCase{"Auto Leveler", "AutoLevelerPlugin", 48000.0F, 1093u,
                 6u, 96u, 0xeffe7a59u, autoLevelerParameters, 2.0e-5F,
                 readGoldenAudio(argv[1], "Auto Leveler")},
      GoldenCase{"Bluetooth SBC", "BluetoothSBCSimulatorPlugin", 48000.0F,
                 4097u, 2u, 128u, 0xeffe7a5eu, bluetoothSbcParameters,
                 2.0e-5F, readGoldenAudio(argv[2], "Bluetooth SBC")},
      GoldenCase{"Cassette Artifacts", "CassetteArtifactsPlugin", 48000.0F,
                 12000u, 2u, 127u, 0xeffe7a5eu, cassetteParameters, 1.0e-5F,
                 readGoldenAudio(argv[3], "Cassette Artifacts")},
      GoldenCase{"Tape Artifacts", "TapeArtifactsPlugin", 48000.0F, 12000u,
                 2u, 127u, 0xeffe7a5eu, tapeParameters, 1.0e-5F,
                 readGoldenAudio(argv[4], "Tape Artifacts")},
      GoldenCase{"Vinyl Artifacts", "VinylArtifactsPlugin", 48000.0F,
                 2049u, 4u, 65u, 0xeffe7a5cu, vinylParameters, 1.0e-5F,
                 readGoldenAudio(argv[5], "Vinyl Artifacts")}};
  for (const auto &testCase : goldenCases) {
    check(testCase.expected.size() ==
              static_cast<std::size_t>(testCase.frameCount) *
                  testCase.channelCount,
          "official golden size must match its case dimensions");
  }
  auto loaded = std::vector<std::pair<std::uint32_t, BackendApi>>{};
  for (auto index = 6; index < argc; ++index) {
    const auto path = std::filesystem::path(argv[index]);
    const auto variant = expectedVariant(path);
    if (!cpuSupports(variant)) {
      continue;
    }
    loaded.emplace_back(variant, loadBackend(path));
  }
  check(!loaded.empty() &&
            loaded.front().first ==
                PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR,
        "the scalar backend must be loaded first");
  if (!loaded.empty()) {
    auto &scalar = loaded.front().second;
    check(scalar.handle != nullptr, "scalar backend must load");
    if (scalar.handle != nullptr && scalar.abiVersion != nullptr &&
        scalar.buildFlags != nullptr && scalar.backendVariant != nullptr) {
      check(scalar.abiVersion() == EFFETUNE_DSP_ABI_VERSION,
            "scalar backend must use the official EffeTune ABI");
      check((scalar.buildFlags() & ET_BUILD_SIMD) == 0u,
            "scalar backend must clear ET_BUILD_SIMD");
      check(scalar.backendVariant() ==
                PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR,
            "scalar backend must report its concrete variant");
      checkAllAbiSymbols(scalar.handle);
      checkEffeTune26Catalog(scalar);
      checkTubeRuntimeContract(scalar);
      const auto scalarSpectrum = renderImpulseSpectrum(scalar);
      checkGoldenCases(scalar, PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR,
                       goldenCases);

      for (auto index = std::size_t{1}; index < loaded.size(); ++index) {
        const auto expected = loaded[index].first;
        auto &simd = loaded[index].second;
        if (simd.handle == nullptr || simd.abiVersion == nullptr ||
            simd.buildFlags == nullptr || simd.backendVariant == nullptr) {
          continue;
        }
        check(simd.abiVersion() == EFFETUNE_DSP_ABI_VERSION,
              "SIMD backend must use the official EffeTune ABI");
        check((simd.buildFlags() & ET_BUILD_SIMD) != 0u,
              "SIMD backend must set ET_BUILD_SIMD");
        check(simd.backendVariant() == expected,
              "SIMD backend must report its concrete variant");
        checkAllAbiSymbols(simd.handle);
        checkCatalogsMatch(scalar, simd);
        checkTubeRuntimeContract(simd);
        checkGoldenCases(simd, expected, goldenCases);

        const auto simdSpectrum = renderImpulseSpectrum(simd);
        check(scalarSpectrum.size() == simdSpectrum.size(),
              "scalar and SIMD FFT sizes must match");
        if (scalarSpectrum.size() == simdSpectrum.size()) {
          for (auto sample = std::size_t{0};
               sample < scalarSpectrum.size(); ++sample) {
            check(std::abs(scalarSpectrum[sample] - simdSpectrum[sample]) <=
                      1.0e-5F,
                  "scalar and SIMD FFT results must remain within tolerance");
          }
        }

        dlerror();
        check(dlsym(simd.handle, "pffft_transform") == nullptr,
              "PFFFT symbols must remain private");
        static_cast<void>(dlerror());
      }

      dlerror();
      check(dlsym(scalar.handle, "et_kernel_descriptor_VolumePlugin") ==
                nullptr,
            "kernel descriptor symbols must remain private");
      static_cast<void>(dlerror());
    }
  }

  for (auto iterator = loaded.rbegin(); iterator != loaded.rend();
       ++iterator) {
    closeBackend(iterator->second);
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d EffeTune backend artifact check(s) failed\n",
                 failures);
    return 1;
  }
  std::puts("All EffeTune backend artifact tests passed");
  return 0;
}
