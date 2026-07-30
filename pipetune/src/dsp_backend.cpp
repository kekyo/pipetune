#include "pipetune/dsp_backend.h"

#include "dsp_backend_loader.h"
#include "dsp_backend_paths.h"

#include <dlfcn.h>

#if defined(__arm__) || defined(__riscv)
#include <sys/auxv.h>
#endif
#if defined(__arm__)
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
  DspBackendKind kind = DspBackendKind::scalar;
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
  create(DspBackendKind kind, const std::filesystem::path &libraryPath,
         void *libraryHandle, DspBackendApi api) {
    auto implementation = std::make_unique<DspBackend::Impl>();
    implementation->kind = kind;
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

static CpuSupport nativeSimdSupport() {
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

static std::string backendFilename(DspBackendKind kind) {
  return kind == DspBackendKind::scalar ? "libeffetune-dsp-scalar.so"
                                        : "libeffetune-dsp-simd.so";
}

static DspBackendLoadResult
loadError(const std::filesystem::path &path, std::string requirement,
          std::string message) {
  return {
      .backend = nullptr,
      .attemptedPath = path,
      .cpuRequirement = std::move(requirement),
      .error = std::move(message),
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
  PIPETUNE_LOAD_DSP_SYMBOL(arenaCombinedPtr, et_arena_combined_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(arenaBusPtr, et_arena_bus_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(arenaScratchPtr, et_arena_scratch_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(scratchPtr, et_scratch_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(telemetryStagingPtr,
                           et_telemetry_staging_ptr)
  PIPETUNE_LOAD_DSP_SYMBOL(telemetryCapacity, et_telemetry_capacity)
  PIPETUNE_LOAD_DSP_SYMBOL(telemetryRead, et_telemetry_read)
  PIPETUNE_LOAD_DSP_SYMBOL(pipelineConfigure, et_pipeline_configure)
  PIPETUNE_LOAD_DSP_SYMBOL(pipelineProcess, et_pipeline_process)
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
  return implementation_->kind;
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

DspBackendLoadResult
loadDspBackendFromPath(DspBackendKind kind,
                       const std::filesystem::path &libraryPath,
                       const DspBackendLoadContext &context) {
  const auto requirement =
      kind == DspBackendKind::simd ? context.simdCpuRequirement : "none";
  if (kind == DspBackendKind::simd && !context.simdCpuSupported) {
    return loadError(
        libraryPath, requirement,
        "SIMD DSP backend requires " + context.simdCpuRequirement +
            ", which is unavailable on this CPU");
  }

  auto *rawHandle = dlopen(libraryPath.c_str(), RTLD_NOW | RTLD_LOCAL);
  if (rawHandle == nullptr) {
    const char *loaderError = dlerror();
    return loadError(
        libraryPath, requirement,
        "cannot load DSP backend " + libraryPath.string() + ": " +
            (loaderError == nullptr ? "unknown dynamic loader error"
                                    : std::string(loaderError)));
  }
  auto handle = LibraryHandle(rawHandle);
  auto api = DspBackendApi{};
  auto symbolError = std::string{};
  if (!loadSymbol(handle.get(), "et_abi_version", api.abiVersion,
                  symbolError)) {
    return loadError(libraryPath, requirement, std::move(symbolError));
  }
  const auto abiVersion = api.abiVersion();
  if (abiVersion != EFFETUNE_DSP_ABI_VERSION) {
    return loadError(
        libraryPath, requirement,
        "DSP backend ABI version " + std::to_string(abiVersion) +
            " does not match required ABI version " +
            std::to_string(EFFETUNE_DSP_ABI_VERSION));
  }
  if (!loadSymbol(handle.get(), "et_build_flags", api.buildFlags,
                  symbolError)) {
    return loadError(libraryPath, requirement, std::move(symbolError));
  }
  const auto simdFlag = (api.buildFlags() & ET_BUILD_SIMD) != 0u;
  if (simdFlag != (kind == DspBackendKind::simd)) {
    return loadError(
        libraryPath, requirement,
        "DSP backend build flags do not match the requested " +
            std::string(dspBackendName(kind)) + " variant");
  }
  if (!loadRemainingSymbols(handle.get(), api, symbolError)) {
    return loadError(libraryPath, requirement, std::move(symbolError));
  }
  const auto catalogError = validateCatalog(api, context.expectedCatalog);
  if (!catalogError.empty()) {
    return loadError(libraryPath, requirement, catalogError);
  }

  auto backend = DspBackendAccess::create(
      kind, libraryPath, handle.release(), std::move(api));
  return {
      .backend = std::move(backend),
      .attemptedPath = libraryPath,
      .cpuRequirement = requirement,
      .error = {},
  };
}

const DspBackendApi &dspBackendApi(const DspBackend &backend) noexcept {
  return DspBackendAccess::api(backend);
}

DspBackendLoadResult loadDspBackend(DspBackendKind kind) {
  const auto cpu = nativeSimdSupport();
  auto pathError = std::error_code{};
  const auto executablePath =
      std::filesystem::read_symlink("/proc/self/exe", pathError);
  if (pathError) {
    return loadError(
        {},
        kind == DspBackendKind::simd ? cpu.requirement : std::string("none"),
        "cannot resolve the PipeTune executable path: " +
            pathError.message());
  }
  const auto executableDirectory = executablePath.parent_path();
  const auto filename = backendFilename(kind);
  const auto buildPath = executableDirectory / filename;
  const auto installedPath =
      (executableDirectory /
       PIPETUNE_INSTALLED_DSP_BACKEND_RELATIVE_PATH / filename)
          .lexically_normal();
  const auto context = DspBackendLoadContext{
      .simdCpuSupported = cpu.supported,
      .simdCpuRequirement = cpu.requirement,
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
    return loadDspBackendFromPath(kind, candidate, context);
  }

  return loadError(
      buildPath,
      kind == DspBackendKind::simd ? cpu.requirement : std::string("none"),
      "DSP backend " + filename +
          " was not found beside the executable or in its private "
          "installation directory");
}

} // namespace pipetune
