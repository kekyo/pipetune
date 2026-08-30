/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipetune/dsp_backend.h"

#include "dsp_backend_loader.h"
#include "dsp_backend_paths.h"

#include <dlfcn.h>

#if defined(__aarch64__) || defined(__arm__) || defined(__riscv)
#include <sys/auxv.h>
#endif
#if defined(__aarch64__) || defined(__arm__)
#include <asm/hwcap.h>
#endif

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace pipetune {

struct DspBackend::Impl {
  DspBackendVariant variant = DspBackendVariant::scalar;
  std::filesystem::path libraryPath;
  void *libraryHandle = nullptr;
  DspBackendApi api;

  ~Impl() {
    if (libraryHandle != nullptr) {
      static_cast<void>(dlclose(libraryHandle));
    }
  }
};

struct DspBackendAccess {
  static std::shared_ptr<const DspBackend>
  create(DspBackendVariant variant,
         const std::filesystem::path &libraryPath,
         void *libraryHandle, DspBackendApi api) {
    auto implementation = std::make_unique<DspBackend::Impl>();
    implementation->variant = variant;
    implementation->libraryPath = libraryPath;
    implementation->libraryHandle = libraryHandle;
    implementation->api = api;
    return std::shared_ptr<const DspBackend>(
        new DspBackend(std::move(implementation)));
  }

  static const DspBackendApi &api(const DspBackend &backend) noexcept {
    return backend.implementation_->api;
  }
};

struct LibraryCloser {
  void operator()(void *handle) const noexcept {
    if (handle != nullptr) {
      static_cast<void>(dlclose(handle));
    }
  }
};

using LibraryHandle = std::unique_ptr<void, LibraryCloser>;

struct CpuSupport {
  bool supported;
  std::string requirement;
};

static CpuSupport nativeVariantSupport(DspBackendVariant variant) {
  if (variant == DspBackendVariant::scalar) {
    return {.supported = true, .requirement = "none"};
  }

  if (variant == DspBackendVariant::x86_64_v3) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) ||             \
    defined(_M_IX86)
#if defined(__GNUC__)
    __builtin_cpu_init();
    return {.supported = __builtin_cpu_supports("x86-64-v3") != 0,
            .requirement = "x86-64-v3"};
#else
    return {.supported = false, .requirement = "x86-64-v3"};
#endif
#else
    return {.supported = false, .requirement = "x86-64-v3"};
#endif
  }

  if (variant == DspBackendVariant::x86_64_v4) {
#if defined(__x86_64__) || defined(_M_X64)
#if defined(__GNUC__)
    __builtin_cpu_init();
    return {.supported = __builtin_cpu_supports("x86-64-v4") != 0,
            .requirement = "x86-64-v4"};
#else
    return {.supported = false, .requirement = "x86-64-v4"};
#endif
#else
    return {.supported = false, .requirement = "x86-64-v4"};
#endif
  }

  if (variant == DspBackendVariant::arm64Sve) {
#if defined(__aarch64__)
    return {.supported = (getauxval(AT_HWCAP) & HWCAP_SVE) != 0u,
            .requirement = "Arm64 SVE"};
#else
    return {.supported = false, .requirement = "Arm64 SVE"};
#endif
  }

#if defined(__x86_64__) || defined(_M_X64)
  return {.supported = true,
          .requirement = "x86-64 SSE2 architectural baseline"};
#elif defined(__i386__) || defined(_M_IX86)
#if defined(__GNUC__)
  __builtin_cpu_init();
  return {.supported = __builtin_cpu_supports("sse2") != 0,
          .requirement = "x86 SSE2"};
#else
  return {.supported = false, .requirement = "x86 SSE2"};
#endif
#elif defined(__aarch64__) || defined(_M_ARM64)
  return {.supported = true,
          .requirement = "AArch64 Advanced SIMD architectural baseline"};
#elif defined(__arm__)
  return {.supported = (getauxval(AT_HWCAP) & HWCAP_NEON) != 0u,
          .requirement = "Arm NEON"};
#elif defined(__riscv) && __riscv_xlen == 64
  constexpr auto vectorHwcap = 1ul << ('V' - 'A');
  return {.supported = (getauxval(AT_HWCAP) & vectorHwcap) != 0u,
          .requirement = "RISC-V RV64GCV"};
#else
  return {.supported = false,
          .requirement = "a supported architecture SIMD instruction set"};
#endif
}

static std::vector<DspBackendVariant> nativeSimdVariants() {
#if defined(__x86_64__) || defined(_M_X64)
  return {DspBackendVariant::simdBaseline, DspBackendVariant::x86_64_v3,
          DspBackendVariant::x86_64_v4};
#elif defined(__i386__) || defined(_M_IX86)
  return {DspBackendVariant::simdBaseline, DspBackendVariant::x86_64_v3};
#elif defined(__aarch64__) || defined(_M_ARM64)
  return {DspBackendVariant::simdBaseline, DspBackendVariant::arm64Sve};
#else
  return {DspBackendVariant::simdBaseline};
#endif
}

static std::uint32_t abiVariant(DspBackendVariant variant) {
  switch (variant) {
  case DspBackendVariant::scalar:
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR;
  case DspBackendVariant::simdBaseline:
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_SIMD_BASELINE;
  case DspBackendVariant::x86_64_v3:
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V3;
  case DspBackendVariant::x86_64_v4:
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_X86_64_V4;
  case DspBackendVariant::arm64Sve:
    return PIPETUNE_EFFETUNE_BACKEND_VARIANT_ARM64_SVE;
  }
  return PIPETUNE_EFFETUNE_BACKEND_VARIANT_SCALAR;
}

static std::string backendFilename(DspBackendVariant variant) {
  switch (variant) {
  case DspBackendVariant::scalar:
    return "libeffetune-dsp-scalar.so";
  case DspBackendVariant::simdBaseline:
    return "libeffetune-dsp-simd.so";
  case DspBackendVariant::x86_64_v3:
    return "libeffetune-dsp-simd-x86-64-v3.so";
  case DspBackendVariant::x86_64_v4:
    return "libeffetune-dsp-simd-x86-64-v4.so";
  case DspBackendVariant::arm64Sve:
    return "libeffetune-dsp-simd-arm64-sve.so";
  }
  return {};
}

static DspBackendLoadResult
loadError(DspBackendVariant variant, const std::filesystem::path &path,
          bool cpuSupported, std::string requirement,
          std::string message) {
  return {
      .backend = nullptr,
      .attemptedPath = path,
      .cpuRequirement = std::move(requirement),
      .error = std::move(message),
      .variant = variant,
      .cpuSupported = cpuSupported,
  };
}

template <typename Function>
static bool loadSymbol(void *handle, const char *name, Function &output,
                       std::string &error) {
  dlerror();
  void *address = dlsym(handle, name);
  const char *loaderError = dlerror();
  if (loaderError != nullptr || address == nullptr) {
    error = "DSP backend is missing required symbol " + std::string(name);
    if (loaderError != nullptr) {
      error += ": ";
      error += loaderError;
    }
    return false;
  }
  static_assert(sizeof(Function) == sizeof(address));
  std::memcpy(&output, &address, sizeof(output));
  return true;
}

static bool loadRemainingSymbols(void *handle, DspBackendApi &api,
                                 std::string &error) {
#define PIPETUNE_LOAD_DSP_SYMBOL(Member, Name)                                \
  if (!loadSymbol(handle, #Name, api.Member, error)) {                        \
    return false;                                                             \
  }
  PIPETUNE_LOAD_DSP_SYMBOL(kernelCount, et_kernel_count)
  PIPETUNE_LOAD_DSP_SYMBOL(kernelName, et_kernel_name)
  PIPETUNE_LOAD_DSP_SYMBOL(kernelParamsHash, et_kernel_params_hash)
  PIPETUNE_LOAD_DSP_SYMBOL(kernelParamBytesCapacity,
                           et_kernel_param_bytes_capacity)
  PIPETUNE_LOAD_DSP_SYMBOL(kernelAssetCapacity, et_kernel_asset_capacity)
  PIPETUNE_LOAD_DSP_SYMBOL(designFftCreate, et_design_fft_create)
  PIPETUNE_LOAD_DSP_SYMBOL(designFftDestroy, et_design_fft_destroy)
  PIPETUNE_LOAD_DSP_SYMBOL(designFftInput, et_design_fft_input)
  PIPETUNE_LOAD_DSP_SYMBOL(designFftOutput, et_design_fft_output)
  PIPETUNE_LOAD_DSP_SYMBOL(designFftForward, et_design_fft_forward)
  PIPETUNE_LOAD_DSP_SYMBOL(designFftInverse, et_design_fft_inverse)
  PIPETUNE_LOAD_DSP_SYMBOL(engineMemoryRequired, et_engine_memory_required)
  PIPETUNE_LOAD_DSP_SYMBOL(engineCreate, et_engine_create)
  PIPETUNE_LOAD_DSP_SYMBOL(engineDestroy, et_engine_destroy)
  PIPETUNE_LOAD_DSP_SYMBOL(enginePrepare, et_engine_prepare)
  PIPETUNE_LOAD_DSP_SYMBOL(engineReset, et_engine_reset)
  PIPETUNE_LOAD_DSP_SYMBOL(engineSetTelemetryRate,
                           et_engine_set_telemetry_rate)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceCreate, et_instance_create)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceDestroy, et_instance_destroy)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceReset, et_instance_reset)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceLatency, et_instance_latency)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceSetTap, et_instance_set_tap)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceSetSeed, et_instance_set_seed)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceSetParams, et_instance_set_params)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceSetParamBytes,
                           et_instance_set_param_bytes)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceAssetBegin, et_instance_asset_begin)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceAssetCommit, et_instance_asset_commit)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceAssetAbort, et_instance_asset_abort)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceAssetState, et_instance_asset_state)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceProcess, et_instance_process)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceRuntimeEvent, et_instance_runtime_event)
  PIPETUNE_LOAD_DSP_SYMBOL(arenaCombinedPtr, et_arena_combined_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(arenaBusPtr, et_arena_bus_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(arenaScratchPtr, et_arena_scratch_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(scratchPtr, et_scratch_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(telemetryStagingPtr,
                           et_telemetry_staging_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(telemetryCapacity, et_telemetry_capacity)
  PIPETUNE_LOAD_DSP_SYMBOL(telemetryRead, et_telemetry_read)
  PIPETUNE_LOAD_DSP_SYMBOL(pipelineConfigure, et_pipeline_configure)
  PIPETUNE_LOAD_DSP_SYMBOL(pipelineLatency, et_pipeline_latency)
  PIPETUNE_LOAD_DSP_SYMBOL(pipelineProcess, et_pipeline_process)
  PIPETUNE_LOAD_DSP_SYMBOL(instanceAssetCopy,
                           pipetune_effetune_instance_asset_copy_v1)
#undef PIPETUNE_LOAD_DSP_SYMBOL
  return true;
}

static std::optional<std::string> kernelName(const DspBackendApi &api,
                                             std::uint32_t index) {
  const auto length = api.kernelName(index, nullptr, 0u);
  if (length <= 0 || length > 4096) {
    return std::nullopt;
  }
  auto buffer =
      std::vector<char>(static_cast<std::size_t>(length) + 1u, '\0');
  const auto copied = api.kernelName(
      index, buffer.data(), static_cast<std::uint32_t>(buffer.size()));
  if (copied != length) {
    return std::nullopt;
  }
  return std::string(buffer.data(), static_cast<std::size_t>(length));
}

static std::string
validateCatalog(const DspBackendApi &api,
                std::span<const DspDefinition> expectedCatalog) {
  if (api.kernelCount() != expectedCatalog.size()) {
    return "DSP backend kernel catalog count does not match PipeTune";
  }
  for (auto index = std::size_t{0}; index < expectedCatalog.size(); ++index) {
    const auto abiIndex = static_cast<std::uint32_t>(index);
    const auto name = kernelName(api, abiIndex);
    const auto &expected = expectedCatalog[index];
    if (!name.has_value() || *name != expected.typeName) {
      return "DSP backend kernel catalog name mismatch at index " +
             std::to_string(index);
    }
    if (api.kernelParamsHash(abiIndex) != expected.hash) {
      return "DSP backend kernel catalog parameter hash mismatch for " +
             *name;
    }
    if (api.kernelParamBytesCapacity(abiIndex) !=
        expected.paramBytesCapacity) {
      return "DSP backend kernel catalog parameter byte capacity mismatch for " +
             *name;
    }
    for (auto slot = std::size_t{0};
         slot < expected.assetCapacities.size(); ++slot) {
      if (api.kernelAssetCapacity(
              abiIndex, static_cast<std::uint32_t>(slot)) !=
          expected.assetCapacities[slot]) {
        return "DSP backend kernel catalog asset capacity mismatch for " +
               *name;
      }
    }
  }
  return {};
}

DspBackend::DspBackend(std::unique_ptr<Impl> implementation)
    : implementation_(std::move(implementation)) {}

DspBackend::~DspBackend() = default;

DspBackendKind DspBackend::kind() const noexcept {
  return dspBackendKind(implementation_->variant);
}

DspBackendVariant DspBackend::variant() const noexcept {
  return implementation_->variant;
}

const std::filesystem::path &DspBackend::libraryPath() const noexcept {
  return implementation_->libraryPath;
}

std::string_view dspBackendName(DspBackendKind kind) noexcept {
  return kind == DspBackendKind::scalar ? std::string_view("scalar")
                                        : std::string_view("simd");
}

std::optional<DspBackendKind>
parseDspBackendName(std::string_view name) noexcept {
  if (name == "scalar") {
    return DspBackendKind::scalar;
  }
  if (name == "simd") {
    return DspBackendKind::simd;
  }
  return std::nullopt;
}

std::string_view
dspBackendVariantName(DspBackendVariant variant) noexcept {
  switch (variant) {
  case DspBackendVariant::scalar:
    return "scalar";
  case DspBackendVariant::simdBaseline:
    return "baseline";
  case DspBackendVariant::x86_64_v3:
    return "x86-64-v3";
  case DspBackendVariant::x86_64_v4:
    return "x86-64-v4";
  case DspBackendVariant::arm64Sve:
    return "sve";
  }
  return {};
}

std::optional<DspBackendVariant>
parseDspBackendVariantName(std::string_view name) noexcept {
  if (name == "scalar") {
    return DspBackendVariant::scalar;
  }
  if (name == "baseline") {
    return DspBackendVariant::simdBaseline;
  }
  if (name == "x86-64-v3") {
    return DspBackendVariant::x86_64_v3;
  }
  if (name == "x86-64-v4") {
    return DspBackendVariant::x86_64_v4;
  }
  if (name == "sve") {
    return DspBackendVariant::arm64Sve;
  }
  return std::nullopt;
}

DspBackendKind
dspBackendKind(DspBackendVariant variant) noexcept {
  return variant == DspBackendVariant::scalar ? DspBackendKind::scalar
                                               : DspBackendKind::simd;
}

std::string_view dspSimdVariantName(DspSimdVariant variant) noexcept {
  switch (variant) {
  case DspSimdVariant::automatic:
    return "auto";
  case DspSimdVariant::baseline:
    return "baseline";
  case DspSimdVariant::x86_64_v3:
    return "x86-64-v3";
  case DspSimdVariant::x86_64_v4:
    return "x86-64-v4";
  case DspSimdVariant::arm64Sve:
    return "sve";
  }
  return {};
}

std::optional<DspSimdVariant>
parseDspSimdVariantName(std::string_view name) noexcept {
  if (name == "auto") {
    return DspSimdVariant::automatic;
  }
  if (name == "baseline") {
    return DspSimdVariant::baseline;
  }
  if (name == "x86-64-v3") {
    return DspSimdVariant::x86_64_v3;
  }
  if (name == "x86-64-v4") {
    return DspSimdVariant::x86_64_v4;
  }
  if (name == "sve") {
    return DspSimdVariant::arm64Sve;
  }
  return std::nullopt;
}

std::optional<DspBackendVariant>
concreteDspBackendVariant(DspSimdVariant variant) noexcept {
  switch (variant) {
  case DspSimdVariant::automatic:
    return std::nullopt;
  case DspSimdVariant::baseline:
    return DspBackendVariant::simdBaseline;
  case DspSimdVariant::x86_64_v3:
    return DspBackendVariant::x86_64_v3;
  case DspSimdVariant::x86_64_v4:
    return DspBackendVariant::x86_64_v4;
  case DspSimdVariant::arm64Sve:
    return DspBackendVariant::arm64Sve;
  }
  return std::nullopt;
}

DspBackendLoadResult
loadDspBackendFromPath(DspBackendVariant variant,
                       const std::filesystem::path &libraryPath,
                       const DspBackendLoadContext &context) {
  const auto kind = dspBackendKind(variant);
  const auto requirement = kind == DspBackendKind::simd
                               ? context.cpuRequirement
                               : std::string("none");
  if (kind == DspBackendKind::simd && !context.cpuSupported) {
    return loadError(
        variant, libraryPath, context.cpuSupported, requirement,
        "SIMD DSP backend requires " + context.cpuRequirement +
            ", which is unavailable on this CPU");
  }

  auto *rawHandle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (rawHandle == nullptr) {
    const char *loaderError = dlerror();
    return loadError(
        variant, libraryPath, context.cpuSupported, requirement,
        "cannot load DSP backend " + libraryPath.string() + ": " +
            (loaderError == nullptr ? "unknown dynamic loader error"
                                    : std::string(loaderError)));
  }
  auto handle = LibraryHandle(rawHandle);
  auto api = DspBackendApi{};
  auto symbolError = std::string{};
  if (!loadSymbol(handle.get(), "et_abi_version", api.abiVersion,
                  symbolError)) {
    return loadError(variant, libraryPath, context.cpuSupported, requirement,
                     std::move(symbolError));
  }
  const auto abiVersion = api.abiVersion();
  if (abiVersion != EFFETUNE_DSP_ABI_VERSION) {
    return loadError(
        variant, libraryPath, context.cpuSupported, requirement,
        "DSP backend ABI version " + std::to_string(abiVersion) +
            " does not match required ABI version " +
            std::to_string(EFFETUNE_DSP_ABI_VERSION));
  }
  if (!loadSymbol(handle.get(), "et_build_flags", api.buildFlags,
                  symbolError)) {
    return loadError(variant, libraryPath, context.cpuSupported, requirement,
                     std::move(symbolError));
  }
  const auto simdFlag = (api.buildFlags() & ET_BUILD_SIMD) != 0u;
  if (simdFlag != (kind == DspBackendKind::simd)) {
    return loadError(
        variant, libraryPath, context.cpuSupported, requirement,
        "DSP backend build flags do not match the requested " +
            std::string(dspBackendName(kind)) + " variant");
  }
  if (!loadSymbol(handle.get(), "pipetune_effetune_backend_variant",
                  api.backendVariant, symbolError)) {
    return loadError(variant, libraryPath, context.cpuSupported, requirement,
                     std::move(symbolError));
  }
  if (api.backendVariant() != abiVariant(variant)) {
    return loadError(
        variant, libraryPath, context.cpuSupported, requirement,
        "DSP backend concrete variant does not match requested " +
            std::string(dspBackendVariantName(variant)));
  }
  if (!loadRemainingSymbols(handle.get(), api, symbolError)) {
    return loadError(variant, libraryPath, context.cpuSupported, requirement,
                     std::move(symbolError));
  }
  const auto catalogError = validateCatalog(api, context.expectedCatalog);
  if (!catalogError.empty()) {
    return loadError(variant, libraryPath, context.cpuSupported, requirement,
                     catalogError);
  }

  auto backend = DspBackendAccess::create(
      variant, libraryPath, handle.release(), std::move(api));
  return {
      .backend = std::move(backend),
      .attemptedPath = libraryPath,
      .cpuRequirement = requirement,
      .error = {},
      .variant = variant,
      .cpuSupported = true,
  };
}

const DspBackendApi &dspBackendApi(const DspBackend &backend) noexcept {
  return DspBackendAccess::api(backend);
}

DspBackendLoadResult loadDspBackend(DspBackendVariant variant) {
  const auto cpu = nativeVariantSupport(variant);
  auto pathError = std::error_code{};
  const auto executablePath =
      std::filesystem::read_symlink("/proc/self/exe", pathError);
  if (pathError) {
    return loadError(
        variant, {}, cpu.supported,
        variant == DspBackendVariant::scalar ? std::string("none")
                                             : cpu.requirement,
        "cannot resolve the PipeTune executable path: " +
            pathError.message());
  }
  const auto executableDirectory = executablePath.parent_path();
  const auto filename = backendFilename(variant);
  const auto buildPath = executableDirectory / filename;
  const auto installedPath =
      (executableDirectory /
       PIPETUNE_INSTALLED_DSP_BACKEND_RELATIVE_PATH / filename)
          .lexically_normal();
  const auto context = DspBackendLoadContext{
      .cpuSupported = cpu.supported,
      .cpuRequirement = cpu.requirement,
      .expectedCatalog = generatedDspCatalog(),
  };

  auto candidates = std::array{buildPath, installedPath};
  auto inspected = std::vector<std::filesystem::path>();
  for (const auto &candidate : candidates) {
    if (std::ranges::find(inspected, candidate) != inspected.end()) {
      continue;
    }
    inspected.push_back(candidate);
    auto fileError = std::error_code{};
    if (!std::filesystem::exists(candidate, fileError) || fileError) {
      continue;
    }
    return loadDspBackendFromPath(variant, candidate, context);
  }

  return loadError(
      variant, buildPath, cpu.supported,
      variant == DspBackendVariant::scalar ? std::string("none")
                                           : cpu.requirement,
      "DSP backend " + filename +
          " was not found beside the executable or in its private "
      "installation directory");
}

DspBackendLoadResult loadDspBackend(DspBackendKind kind) {
  if (kind == DspBackendKind::scalar) {
    return loadDspBackend(DspBackendVariant::scalar);
  }

  auto variants = nativeSimdVariants();
  auto unavailable = DspBackendLoadResult{
      .backend = nullptr,
      .attemptedPath = {},
      .cpuRequirement = {},
      .error = "SIMD DSP backend is unavailable",
      .variant = DspBackendVariant::simdBaseline,
  };
  for (auto iterator = variants.rbegin(); iterator != variants.rend();
       ++iterator) {
    auto result = loadDspBackend(*iterator);
    if (result.backend != nullptr) {
      return result;
    }
    unavailable = std::move(result);
  }
  return unavailable;
}

const DspBackendLoadResult &
DspBackends::get(DspBackendKind kind) const noexcept {
  return kind == DspBackendKind::scalar ? scalar : simd;
}

DspBackendLoadResult const *
DspBackends::find(DspBackendVariant variant) const noexcept {
  if (variant == DspBackendVariant::scalar) {
    return &scalar;
  }
  const auto found = std::ranges::find_if(
      simdVariants, [variant](const DspBackendLoadResult &result) {
        return result.variant == variant;
      });
  return found == simdVariants.end() ? nullptr : &*found;
}

DspBackends discoverDspBackends() {
  auto backends = DspBackends{};
  backends.scalar = loadDspBackend(DspBackendVariant::scalar);
  for (const auto variant : nativeSimdVariants()) {
    backends.simdVariants.push_back(loadDspBackend(variant));
  }
  if (!backends.simdVariants.empty()) {
    backends.simd = backends.simdVariants.back();
    for (const auto &candidate : backends.simdVariants) {
      if (candidate.backend != nullptr) {
        backends.simd = candidate;
      }
    }
  }
  return backends;
}

DspBackendSelection
selectDspBackend(DspBackendKind configuredBackend,
                 const DspBackends &backends) {
  return selectDspBackend(configuredBackend, DspSimdVariant::automatic,
                          backends);
}

static std::string unavailableVariantError(
    DspBackendVariant variant, const DspBackendLoadResult *loaded) {
  if (loaded != nullptr && !loaded->error.empty()) {
    return loaded->error;
  }
  return "DSP SIMD variant " +
         std::string(dspBackendVariantName(variant)) +
         " is unavailable on this architecture";
}

DspBackendSelection
selectDspBackend(DspBackendKind configuredBackend,
                 DspSimdVariant configuredSimdVariant,
                 const DspBackends &backends) {
  if (backends.scalar.backend == nullptr) {
    auto error = backends.scalar.error;
    if (error.empty()) {
      error = "scalar DSP backend is unavailable";
    }
    return {
        .configuredBackend = configuredBackend,
        .configuredSimdVariant = configuredSimdVariant,
        .effectiveBackend = nullptr,
        .effectiveVariant = std::nullopt,
        .fallback = false,
        .error = std::move(error),
    };
  }
  if (configuredBackend == DspBackendKind::scalar) {
    return {
        .configuredBackend = configuredBackend,
        .configuredSimdVariant = configuredSimdVariant,
        .effectiveBackend = backends.scalar.backend,
        .effectiveVariant = DspBackendVariant::scalar,
        .fallback = false,
        .error = {},
    };
  }
  if (configuredBackend != DspBackendKind::simd ||
      dspSimdVariantName(configuredSimdVariant).empty()) {
    return {
        .configuredBackend = configuredBackend,
        .configuredSimdVariant = configuredSimdVariant,
        .effectiveBackend = backends.scalar.backend,
        .effectiveVariant = DspBackendVariant::scalar,
        .fallback = true,
        .error = "configured DSP backend selection is invalid",
    };
  }

  const auto pinned = concreteDspBackendVariant(configuredSimdVariant);
  if (pinned.has_value()) {
    const auto *requested = backends.find(*pinned);
    if (requested != nullptr && requested->backend != nullptr) {
      return {
          .configuredBackend = configuredBackend,
          .configuredSimdVariant = configuredSimdVariant,
          .effectiveBackend = requested->backend,
          .effectiveVariant = *pinned,
          .fallback = false,
          .error = {},
      };
    }
    return {
        .configuredBackend = configuredBackend,
        .configuredSimdVariant = configuredSimdVariant,
        .effectiveBackend = backends.scalar.backend,
        .effectiveVariant = DspBackendVariant::scalar,
        .fallback = true,
        .error = unavailableVariantError(*pinned, requested),
    };
  }

  auto supportedFailure = std::string{};
  for (auto iterator = backends.simdVariants.rbegin();
       iterator != backends.simdVariants.rend(); ++iterator) {
    if (!iterator->cpuSupported) {
      continue;
    }
    if (iterator->backend != nullptr) {
      return {
          .configuredBackend = configuredBackend,
          .configuredSimdVariant = configuredSimdVariant,
          .effectiveBackend = iterator->backend,
          .effectiveVariant = iterator->variant,
          .fallback = !supportedFailure.empty(),
          .error = std::move(supportedFailure),
      };
    }
    if (supportedFailure.empty()) {
      supportedFailure = unavailableVariantError(iterator->variant,
                                                 &*iterator);
    }
  }
  if (supportedFailure.empty()) {
    supportedFailure = backends.simd.error.empty()
                           ? "SIMD DSP backend is unavailable"
                           : backends.simd.error;
  }
  return {
      .configuredBackend = configuredBackend,
      .configuredSimdVariant = configuredSimdVariant,
      .effectiveBackend = backends.scalar.backend,
      .effectiveVariant = DspBackendVariant::scalar,
      .fallback = true,
      .error = std::move(supportedFailure),
  };
}

} // namespace pipetune
