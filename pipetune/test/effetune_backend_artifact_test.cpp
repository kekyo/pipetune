#include <effetune/abi.h>

#include <dlfcn.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

static int failures = 0;

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

static void checkAllAbiSymbols(void *handle) {
  static constexpr std::array names = {
      "et_abi_version",
      "et_build_flags",
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
      "et_arena_combined_ptr",
      "et_arena_bus_ptr",
      "et_arena_scratch_ptr",
      "et_scratch_ptr",
      "et_telemetry_staging_ptr",
      "et_telemetry_capacity",
      "et_telemetry_read",
      "et_pipeline_configure",
      "et_pipeline_process"};
  for (const char *name : names) {
    dlerror();
    void *address = dlsym(handle, name);
    check(dlerror() == nullptr && address != nullptr,
          "every EffeTune ABI v1 symbol must be exported");
  }
}

int main(int argc, char **argv) {
  if (argc != 3) {
    std::fprintf(stderr,
                 "usage: effetune_backend_artifact_test SCALAR_SO SIMD_SO\n");
    return 2;
  }
  const auto scalarPath = std::filesystem::path(argv[1]);
  const auto simdPath = std::filesystem::path(argv[2]);
  check(scalarPath.filename() == "libeffetune-dsp-scalar.so",
        "scalar backend filename must be stable");
  check(simdPath.filename() == "libeffetune-dsp-simd.so",
        "SIMD backend filename must be stable");

  auto scalar = loadBackend(scalarPath);
  auto simd = loadBackend(simdPath);
  if (scalar.handle != nullptr && simd.handle != nullptr &&
      scalar.abiVersion != nullptr && simd.abiVersion != nullptr &&
      scalar.buildFlags != nullptr && simd.buildFlags != nullptr) {
    check(scalar.abiVersion() == EFFETUNE_DSP_ABI_VERSION,
          "scalar backend ABI version must match");
    check(simd.abiVersion() == EFFETUNE_DSP_ABI_VERSION,
          "SIMD backend ABI version must match");
    check((scalar.buildFlags() & ET_BUILD_SIMD) == 0u,
          "scalar backend must clear ET_BUILD_SIMD");
    check((simd.buildFlags() & ET_BUILD_SIMD) != 0u,
          "SIMD backend must set ET_BUILD_SIMD");
    checkAllAbiSymbols(scalar.handle);
    checkAllAbiSymbols(simd.handle);
    checkCatalogsMatch(scalar, simd);

    const auto scalarSpectrum = renderImpulseSpectrum(scalar);
    const auto simdSpectrum = renderImpulseSpectrum(simd);
    check(scalarSpectrum.size() == simdSpectrum.size(),
          "scalar and SIMD FFT sizes must match");
    if (scalarSpectrum.size() == simdSpectrum.size()) {
      for (auto index = std::size_t{0}; index < scalarSpectrum.size();
           ++index) {
        check(std::abs(scalarSpectrum[index] - simdSpectrum[index]) <= 1.0e-5F,
              "scalar and SIMD FFT results must remain within tolerance");
      }
    }

    dlerror();
    check(dlsym(scalar.handle, "et_kernel_descriptor_VolumePlugin") == nullptr,
          "kernel descriptor symbols must remain private");
    static_cast<void>(dlerror());
    dlerror();
    check(dlsym(simd.handle, "pffft_transform") == nullptr,
          "PFFFT symbols must remain private");
    static_cast<void>(dlerror());
  }

  closeBackend(simd);
  closeBackend(scalar);
  if (failures != 0) {
    std::fprintf(stderr, "%d EffeTune backend artifact check(s) failed\n",
                 failures);
    return 1;
  }
  std::puts("All EffeTune backend artifact tests passed");
  return 0;
}
