#include "pipetune/dsp_backend.h"
#include "pipetune/dsp_pipeline.h"

#include "dsp_backend_loader.h"
#include "dsp_catalog.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <vector>

static bool fail(std::string_view message) {
  std::cerr << message << '\n';
  return false;
}

static bool check(bool condition, std::string_view message) {
  return condition ? true : fail(message);
}

static bool contains(std::string_view value, std::string_view expected) {
  return value.find(expected) != std::string_view::npos;
}

static pipetune::DspBackendLoadContext loadContext(bool simdCpuSupported) {
  return {
      .simdCpuSupported = simdCpuSupported,
      .simdCpuRequirement = "test SIMD ISA",
      .expectedCatalog = pipetune::generatedDspCatalog(),
  };
}

static bool testNames() {
  return check(pipetune::dspBackendName(pipetune::DspBackendKind::scalar) ==
                   "scalar",
               "scalar backend name must be stable") &&
         check(pipetune::dspBackendName(pipetune::DspBackendKind::simd) ==
                   "simd",
               "SIMD backend name must be stable") &&
         check(pipetune::parseDspBackendName("scalar") ==
                   pipetune::DspBackendKind::scalar,
               "scalar backend name must parse") &&
         check(pipetune::parseDspBackendName("simd") ==
                   pipetune::DspBackendKind::simd,
               "SIMD backend name must parse") &&
         check(!pipetune::parseDspBackendName("SIMD").has_value(),
               "backend names must be case-sensitive");
}

static bool testValidBackends(const std::filesystem::path &scalarPath,
                              const std::filesystem::path &simdPath) {
  auto scalar = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::scalar, scalarPath, loadContext(true));
  auto simd = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::simd, simdPath, loadContext(true));
  const auto packagedScalar =
      pipetune::loadDspBackend(pipetune::DspBackendKind::scalar);
  const auto packagedSimd =
      pipetune::loadDspBackend(pipetune::DspBackendKind::simd);
  return check(scalar.backend != nullptr, scalar.error) &&
         check(simd.backend != nullptr, simd.error) &&
         check(packagedScalar.backend != nullptr, packagedScalar.error) &&
         check(packagedSimd.backend != nullptr, packagedSimd.error) &&
         check(scalar.backend->kind() == pipetune::DspBackendKind::scalar,
               "scalar backend must retain its kind") &&
         check(simd.backend->kind() == pipetune::DspBackendKind::simd,
               "SIMD backend must retain its kind") &&
         check(scalar.backend->libraryPath() == scalarPath,
               "scalar backend must retain its exact library path") &&
         check(simd.backend->libraryPath() == simdPath,
               "SIMD backend must retain its exact library path");
}

static bool testRejectedBackends(
    const std::filesystem::path &scalarPath,
    const std::filesystem::path &simdPath,
    const std::filesystem::path &wrongAbiPath,
    const std::filesystem::path &missingSymbolPath) {
  const auto unsupported = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::simd, "/path/that/must/not/be/opened.so",
      loadContext(false));
  if (!check(unsupported.backend == nullptr,
             "unsupported SIMD ISA must reject the backend") ||
      !check(contains(unsupported.error, "test SIMD ISA"),
             "unsupported SIMD diagnostic must name its ISA requirement")) {
    return false;
  }

  const auto wrongAbi = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::scalar, wrongAbiPath, loadContext(true));
  if (!check(wrongAbi.backend == nullptr,
             "an incompatible ABI must reject the backend") ||
      !check(contains(wrongAbi.error, "ABI"),
             "an incompatible ABI must report an ABI diagnostic")) {
    return false;
  }

  const auto missingSymbol = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::scalar, missingSymbolPath, loadContext(true));
  if (!check(missingSymbol.backend == nullptr,
             "a missing ABI symbol must reject the backend") ||
      !check(contains(missingSymbol.error, "et_kernel_count"),
             "a missing ABI symbol diagnostic must identify the symbol")) {
    return false;
  }

  const auto wrongScalarFlag = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::scalar, simdPath, loadContext(true));
  const auto wrongSimdFlag = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::simd, scalarPath, loadContext(true));
  if (!check(wrongScalarFlag.backend == nullptr,
             "the SIMD artifact must not load as scalar") ||
      !check(wrongSimdFlag.backend == nullptr,
             "the scalar artifact must not load as SIMD") ||
      !check(contains(wrongScalarFlag.error, "build flags") &&
                 contains(wrongSimdFlag.error, "build flags"),
             "variant mismatches must report build flags")) {
    return false;
  }

  auto mismatchedCatalog = std::vector<pipetune::DspDefinition>(
      pipetune::generatedDspCatalog().begin(),
      pipetune::generatedDspCatalog().end());
  mismatchedCatalog.front().hash ^= 1u;
  const auto catalogMismatch = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::scalar, scalarPath,
      {
          .simdCpuSupported = true,
          .simdCpuRequirement = "test SIMD ISA",
          .expectedCatalog = mismatchedCatalog,
      });
  return check(catalogMismatch.backend == nullptr,
               "an incompatible kernel catalog must reject the backend") &&
         check(contains(catalogMismatch.error, "catalog"),
               "an incompatible kernel catalog must report its cause");
}

static std::filesystem::path
writeVolumePreset(const std::filesystem::path &directory) {
  const auto path = directory / "backend-volume.effetune_preset";
  auto stream = std::ofstream(path, std::ios::binary);
  stream << R"json({"pipeline":[{"name":"Volume","enabled":true,"parameters":{"vl":-6},"channel":"A"}]})json";
  return path;
}

static bool approximately(float left, float right) {
  return std::abs(left - right) <= 1.0e-6F;
}

static bool testPipelineOwnership(const std::filesystem::path &scalarPath,
                                  const std::filesystem::path &simdPath) {
  auto scalar = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::scalar, scalarPath, loadContext(true));
  auto simd = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendKind::simd, simdPath, loadContext(true));
  if (!check(scalar.backend != nullptr, scalar.error) ||
      !check(simd.backend != nullptr, simd.error)) {
    return false;
  }

  const auto directory =
      std::filesystem::temp_directory_path() /
      ("pipetune-dsp-backend-test-" +
       std::to_string(static_cast<long long>(getpid())));
  std::filesystem::create_directories(directory);
  const auto preset = writeVolumePreset(directory);
  auto scalarPipeline = pipetune::loadDspPipeline(
      preset,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64},
      std::move(scalar.backend));
  auto simdPipeline = pipetune::loadDspPipeline(
      preset,
      {.sampleRate = 48000.0F, .maxChannels = 2, .maxFrames = 64},
      std::move(simd.backend));
  std::filesystem::remove_all(directory);
  if (!check(scalarPipeline.pipeline != nullptr, scalarPipeline.error) ||
      !check(simdPipeline.pipeline != nullptr, simdPipeline.error) ||
      !check(scalarPipeline.pipeline->backendKind() ==
                 pipetune::DspBackendKind::scalar,
             "scalar pipeline must report its backend") ||
      !check(simdPipeline.pipeline->backendKind() ==
                 pipetune::DspBackendKind::simd,
             "SIMD pipeline must report its backend")) {
    return false;
  }

  auto scalarSamples =
      std::vector<float>{1.0F, -0.5F, 0.25F, -1.0F, 0.5F, -0.25F};
  auto simdSamples = scalarSamples;
  if (!check(scalarPipeline.pipeline->process(scalarSamples, 2, 3, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "scalar backend processing failed after external ownership ended") ||
      !check(simdPipeline.pipeline->process(simdSamples, 2, 3, 0.0) ==
                 pipetune::ProcessStatus::ok,
             "SIMD backend processing failed after external ownership ended")) {
    return false;
  }
  return check(std::ranges::equal(
                   scalarSamples, simdSamples,
                   [](float left, float right) {
                     return approximately(left, right);
                   }),
               "scalar and SIMD pipelines must produce equivalent PCM");
}

int main(int argc, char **argv) {
  if (argc != 5) {
    std::cerr
        << "usage: dsp_backend_test SCALAR_SO SIMD_SO WRONG_ABI_SO MISSING_SYMBOL_SO\n";
    return 2;
  }
  const auto scalarPath = std::filesystem::path(argv[1]);
  const auto simdPath = std::filesystem::path(argv[2]);
  const auto wrongAbiPath = std::filesystem::path(argv[3]);
  const auto missingSymbolPath = std::filesystem::path(argv[4]);
  const auto passed =
      testNames() && testValidBackends(scalarPath, simdPath) &&
      testRejectedBackends(scalarPath, simdPath, wrongAbiPath,
                           missingSymbolPath) &&
      testPipelineOwnership(scalarPath, simdPath);
  return passed ? 0 : 1;
}
