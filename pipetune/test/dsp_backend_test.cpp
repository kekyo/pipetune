/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
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

static pipetune::DspBackendLoadContext loadContext(bool cpuSupported) {
  return {
      .cpuSupported = cpuSupported,
      .cpuRequirement = "test SIMD ISA",
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
               "backend names must be case-sensitive") &&
         check(pipetune::dspBackendVariantName(
                   pipetune::DspBackendVariant::simdBaseline) == "baseline",
               "baseline variant name must be stable") &&
         check(pipetune::dspBackendVariantName(
                   pipetune::DspBackendVariant::x86_64_v3) == "x86-64-v3",
               "x86-64-v3 variant name must be stable") &&
         check(pipetune::dspBackendVariantName(
                   pipetune::DspBackendVariant::x86_64_v4) == "x86-64-v4",
               "x86-64-v4 variant name must be stable") &&
         check(pipetune::dspBackendVariantName(
                   pipetune::DspBackendVariant::arm64Sve) == "sve",
               "SVE variant name must be stable");
}

static bool testValidBackends(const std::filesystem::path &scalarPath,
                              const std::filesystem::path &simdPath) {
  auto scalar = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::scalar, scalarPath, loadContext(true));
  auto simd = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::simdBaseline, simdPath, loadContext(true));
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
         check(scalar.backend->variant() ==
                   pipetune::DspBackendVariant::scalar,
               "scalar backend must retain its concrete variant") &&
         check(simd.backend->variant() ==
                   pipetune::DspBackendVariant::simdBaseline,
               "SIMD backend must retain its concrete variant") &&
         check(scalar.backend->libraryPath() == scalarPath,
               "scalar backend must retain its exact library path") &&
         check(simd.backend->libraryPath() == simdPath,
               "SIMD backend must retain its exact library path");
}

static bool testBackendDiscoveryAndSelection() {
  const auto discovered = pipetune::discoverDspBackends();
  if (!check(discovered.scalar.backend != nullptr,
             discovered.scalar.error) ||
      !check(discovered.simd.backend != nullptr, discovered.simd.error)) {
    return false;
  }
  const pipetune::DspBackendLoadResult *highestAvailable = nullptr;
  for (const auto &variant : discovered.simdVariants) {
    if (variant.backend != nullptr) {
      highestAvailable = &variant;
    }
  }
  if (!check(highestAvailable != nullptr,
             "at least one SIMD variant must be available") ||
      !check(discovered.simd.backend == highestAvailable->backend,
             "automatic SIMD selection must use the highest available variant")) {
    return false;
  }

  const auto scalar =
      pipetune::selectDspBackend(pipetune::DspBackendKind::scalar,
                                 discovered);
  const auto simd =
      pipetune::selectDspBackend(pipetune::DspBackendKind::simd,
                                 discovered);
  if (!check(scalar.effectiveBackend != nullptr &&
                 scalar.effectiveBackend->kind() ==
                     pipetune::DspBackendKind::scalar &&
                 !scalar.fallback && scalar.error.empty(),
             "available scalar selection differs") ||
      !check(simd.effectiveBackend != nullptr &&
                 simd.effectiveBackend->kind() ==
                     pipetune::DspBackendKind::simd &&
                 !simd.fallback && simd.error.empty(),
             "available SIMD selection differs")) {
    return false;
  }

  auto withoutSimd = discovered;
  withoutSimd.simd.backend.reset();
  withoutSimd.simd.error = "test SIMD backend is unavailable";
  for (auto &variant : withoutSimd.simdVariants) {
    variant.backend.reset();
    variant.error = "test SIMD backend is unavailable";
  }
  const auto fallback =
      pipetune::selectDspBackend(pipetune::DspBackendKind::simd,
                                 withoutSimd);
  if (!check(fallback.effectiveBackend != nullptr &&
                 fallback.effectiveBackend->kind() ==
                     pipetune::DspBackendKind::scalar,
             "unavailable SIMD must fall back to scalar") ||
      !check(fallback.fallback,
             "unavailable SIMD must report fallback") ||
      !check(contains(fallback.error, "test SIMD backend is unavailable"),
             "SIMD fallback must retain its availability diagnostic")) {
    return false;
  }

  auto withoutScalar = discovered;
  withoutScalar.scalar.backend.reset();
  withoutScalar.scalar.error = "test scalar backend is unavailable";
  const auto unusable =
      pipetune::selectDspBackend(pipetune::DspBackendKind::simd,
                                 withoutScalar);
  return check(unusable.effectiveBackend == nullptr,
               "missing mandatory scalar backend must reject DSP startup") &&
         check(!unusable.fallback,
               "missing scalar backend must not report a usable fallback") &&
         check(contains(unusable.error, "test scalar backend is unavailable"),
               "missing scalar diagnostic must be retained");
}

static bool testAutomaticTierFallback() {
  const auto discovered = pipetune::discoverDspBackends();
  const auto lower = std::ranges::find_if(
      discovered.simdVariants,
      [](const pipetune::DspBackendLoadResult &variant) {
        return variant.backend != nullptr;
      });
  if (!check(discovered.scalar.backend != nullptr,
             discovered.scalar.error) ||
      !check(lower != discovered.simdVariants.end(),
             "automatic tier fallback requires one usable SIMD backend")) {
    return false;
  }

  auto unsupportedUpper = pipetune::DspBackendLoadResult{
      .backend = nullptr,
      .attemptedPath = "/test/unsupported-upper.so",
      .cpuRequirement = "test upper ISA",
      .error = "test upper ISA is unsupported",
      .variant = pipetune::DspBackendVariant::x86_64_v4,
      .cpuSupported = false,
  };
  auto backends = pipetune::DspBackends{
      .scalar = discovered.scalar,
      .simd = *lower,
      .simdVariants = {*lower, unsupportedUpper},
  };
  const auto ignored = pipetune::selectDspBackend(
      pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::automatic, backends);
  if (!check(ignored.effectiveBackend == lower->backend &&
                 ignored.effectiveVariant == lower->variant &&
                 !ignored.fallback && ignored.error.empty(),
             "CPU-unsupported upper SIMD tier must be ignored")) {
    return false;
  }

  backends.simdVariants.back().cpuSupported = true;
  backends.simdVariants.back().error =
      "test supported upper SIMD tier is broken";
  const auto degraded = pipetune::selectDspBackend(
      pipetune::DspBackendKind::simd,
      pipetune::DspSimdVariant::automatic, backends);
  return check(degraded.effectiveBackend == lower->backend &&
                   degraded.effectiveVariant == lower->variant &&
                   degraded.fallback &&
                   degraded.error ==
                       "test supported upper SIMD tier is broken",
               "broken CPU-supported upper tier must use lower SIMD with a "
               "fallback diagnostic");
}

static bool testRejectedBackends(
    const std::filesystem::path &scalarPath,
    const std::filesystem::path &simdPath,
    const std::filesystem::path &wrongAbiPath,
    const std::filesystem::path &missingSymbolPath) {
  const auto unsupported = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::x86_64_v3,
      "/path/that/must/not/be/opened.so",
      loadContext(false));
  if (!check(unsupported.backend == nullptr,
             "unsupported SIMD ISA must reject the backend") ||
      !check(contains(unsupported.error, "test SIMD ISA"),
             "unsupported SIMD diagnostic must name its ISA requirement")) {
    return false;
  }

  const auto wrongAbi = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::scalar, wrongAbiPath, loadContext(true));
  if (!check(wrongAbi.backend == nullptr,
             "an incompatible ABI must reject the backend") ||
      !check(contains(wrongAbi.error, "ABI"),
             "an incompatible ABI must report an ABI diagnostic")) {
    return false;
  }

  const auto missingSymbol = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::scalar, missingSymbolPath,
      loadContext(true));
  if (!check(missingSymbol.backend == nullptr,
             "a missing ABI symbol must reject the backend") ||
      !check(contains(missingSymbol.error, "et_kernel_count"),
             "a missing ABI symbol diagnostic must identify the symbol")) {
    return false;
  }

  const auto wrongScalarFlag = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::scalar, simdPath, loadContext(true));
  const auto wrongSimdFlag = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::simdBaseline, scalarPath,
      loadContext(true));
  if (!check(wrongScalarFlag.backend == nullptr,
             "the SIMD artifact must not load as scalar") ||
      !check(wrongSimdFlag.backend == nullptr,
             "the scalar artifact must not load as SIMD") ||
      !check(contains(wrongScalarFlag.error, "build flags") &&
                 contains(wrongSimdFlag.error, "build flags"),
             "variant mismatches must report build flags")) {
    return false;
  }

  const auto wrongConcreteVariant = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::x86_64_v3, simdPath, loadContext(true));
  if (!check(wrongConcreteVariant.backend == nullptr,
             "a concrete SIMD variant mismatch must reject the backend") ||
      !check(contains(wrongConcreteVariant.error, "variant"),
             "a concrete SIMD mismatch must identify the variant")) {
    return false;
  }

  auto mismatchedCatalog = std::vector<pipetune::DspDefinition>(
      pipetune::generatedDspCatalog().begin(),
      pipetune::generatedDspCatalog().end());
  mismatchedCatalog.front().hash ^= 1u;
  const auto catalogMismatch = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::scalar, scalarPath,
      {
          .cpuSupported = true,
          .cpuRequirement = "test SIMD ISA",
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
      pipetune::DspBackendVariant::scalar, scalarPath, loadContext(true));
  auto simd = pipetune::loadDspBackendFromPath(
      pipetune::DspBackendVariant::simdBaseline, simdPath, loadContext(true));
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
      testBackendDiscoveryAndSelection() &&
      testAutomaticTierFallback() &&
      testRejectedBackends(scalarPath, simdPath, wrongAbiPath,
                           missingSymbolPath) &&
      testPipelineOwnership(scalarPath, simdPath);
  return passed ? 0 : 1;
}
