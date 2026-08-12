#ifndef PIPETUNE_DSP_BACKEND_LOADER_H
#define PIPETUNE_DSP_BACKEND_LOADER_H

#include "pipetune/dsp_backend.h"

#include "dsp_catalog.h"
#include "effetune_backend_abi.h"

#include <effetune/abi.h>

#include <span>
#include <string>

namespace pipetune {

struct DspBackendApi {
  decltype(&et_abi_version) abiVersion = nullptr;
  decltype(&et_build_flags) buildFlags = nullptr;
  decltype(&pipetune_effetune_backend_variant) backendVariant = nullptr;
  decltype(&et_kernel_count) kernelCount = nullptr;
  decltype(&et_kernel_name) kernelName = nullptr;
  decltype(&et_kernel_params_hash) kernelParamsHash = nullptr;
  decltype(&et_kernel_param_bytes_capacity) kernelParamBytesCapacity = nullptr;
  decltype(&et_kernel_asset_capacity) kernelAssetCapacity = nullptr;
  decltype(&et_design_fft_create) designFftCreate = nullptr;
  decltype(&et_design_fft_destroy) designFftDestroy = nullptr;
  decltype(&et_design_fft_input) designFftInput = nullptr;
  decltype(&et_design_fft_output) designFftOutput = nullptr;
  decltype(&et_design_fft_forward) designFftForward = nullptr;
  decltype(&et_design_fft_inverse) designFftInverse = nullptr;
  decltype(&et_engine_memory_required) engineMemoryRequired = nullptr;
  decltype(&et_engine_create) engineCreate = nullptr;
  decltype(&et_engine_destroy) engineDestroy = nullptr;
  decltype(&et_engine_prepare) enginePrepare = nullptr;
  decltype(&et_engine_reset) engineReset = nullptr;
  decltype(&et_engine_set_telemetry_rate) engineSetTelemetryRate = nullptr;
  decltype(&et_instance_create) instanceCreate = nullptr;
  decltype(&et_instance_destroy) instanceDestroy = nullptr;
  decltype(&et_instance_reset) instanceReset = nullptr;
  decltype(&et_instance_latency) instanceLatency = nullptr;
  decltype(&et_instance_set_tap) instanceSetTap = nullptr;
  decltype(&et_instance_set_seed) instanceSetSeed = nullptr;
  decltype(&et_instance_set_params) instanceSetParams = nullptr;
  decltype(&et_instance_set_param_bytes) instanceSetParamBytes = nullptr;
  decltype(&et_instance_asset_begin) instanceAssetBegin = nullptr;
  decltype(&et_instance_asset_commit) instanceAssetCommit = nullptr;
  decltype(&et_instance_asset_abort) instanceAssetAbort = nullptr;
  decltype(&et_instance_asset_state) instanceAssetState = nullptr;
  decltype(&et_instance_process) instanceProcess = nullptr;
  decltype(&et_instance_runtime_event) instanceRuntimeEvent = nullptr;
  decltype(&et_arena_combined_ptr) arenaCombinedPtr = nullptr;
  decltype(&et_arena_bus_ptr) arenaBusPtr = nullptr;
  decltype(&et_arena_scratch_ptr) arenaScratchPtr = nullptr;
  decltype(&et_scratch_ptr) scratchPtr = nullptr;
  decltype(&et_telemetry_staging_ptr) telemetryStagingPtr = nullptr;
  decltype(&et_telemetry_capacity) telemetryCapacity = nullptr;
  decltype(&et_telemetry_read) telemetryRead = nullptr;
  decltype(&et_pipeline_configure) pipelineConfigure = nullptr;
  decltype(&et_pipeline_process) pipelineProcess = nullptr;
};

struct DspBackendLoadContext {
  bool cpuSupported;
  std::string cpuRequirement;
  std::span<const DspDefinition> expectedCatalog;
};

DspBackendLoadResult
loadDspBackendFromPath(DspBackendVariant variant,
                       const std::filesystem::path &libraryPath,
                       const DspBackendLoadContext &context);

const DspBackendApi &dspBackendApi(const DspBackend &backend) noexcept;

} // namespace pipetune

#endif
