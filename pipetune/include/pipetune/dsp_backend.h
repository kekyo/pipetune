/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#ifndef PIPETUNE_DSP_BACKEND_H
#define PIPETUNE_DSP_BACKEND_H

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pipetune {

/**
 * Identifies one native EffeTune DSP implementation variant.
 */
enum class DspBackendKind {
  /** Scalar PFFFT with the normal compiler optimization policy. */
  scalar,
  /** Architecture SIMD PFFFT with safe compiler auto-vectorization. */
  simd
};

/**
 * Identifies one concrete native DSP shared-library implementation.
 */
enum class DspBackendVariant {
  /** Scalar PFFFT and the normal compiler optimization policy. */
  scalar,
  /** Architecture SIMD PFFFT and baseline auto-vectorization. */
  simdBaseline,
  /** x86-64-v3 auto-vectorization and the x86 PFFFT SIMD implementation. */
  x86_64_v3,
  /** x86-64-v4 auto-vectorization and the x86 PFFFT SIMD implementation. */
  x86_64_v4,
  /** Arm SVE auto-vectorization and the AArch64 PFFFT NEON implementation. */
  arm64Sve
};

/**
 * Selects automatic or pinned SIMD instruction-set dispatch.
 */
enum class DspSimdVariant {
  /** Select the highest usable SIMD variant at runtime. */
  automatic,
  /** Pin architecture-baseline SIMD. */
  baseline,
  /** Pin the x86-64-v3 backend. */
  x86_64_v3,
  /** Pin the x86-64-v4 backend. */
  x86_64_v4,
  /** Pin the Arm64 SVE backend. */
  arm64Sve
};

/**
 * Returns the stable configuration name for a DSP backend kind.
 *
 * @param kind Backend kind.
 * @return Either `scalar` or `simd`.
 */
std::string_view dspBackendName(DspBackendKind kind) noexcept;

/**
 * Parses an exact, case-sensitive DSP backend configuration name.
 *
 * @param name Candidate configuration value.
 * @return Parsed kind, or no value when the name is unsupported.
 */
std::optional<DspBackendKind>
parseDspBackendName(std::string_view name) noexcept;

/**
 * Returns the stable configuration name for a concrete DSP backend variant.
 *
 * @param variant Concrete backend variant.
 * @return `scalar`, `baseline`, `x86-64-v3`, `x86-64-v4`, or `sve`.
 */
std::string_view
dspBackendVariantName(DspBackendVariant variant) noexcept;

/**
 * Parses an exact, case-sensitive concrete DSP backend variant name.
 *
 * @param name Candidate configuration value.
 * @return Parsed variant, or no value when the name is unsupported.
 */
std::optional<DspBackendVariant>
parseDspBackendVariantName(std::string_view name) noexcept;

/**
 * Returns the logical scalar or SIMD kind for a concrete backend variant.
 *
 * @param variant Concrete backend variant.
 * @return Scalar for the scalar variant, otherwise SIMD.
 */
DspBackendKind
dspBackendKind(DspBackendVariant variant) noexcept;

/**
 * Returns the stable configuration name for a SIMD dispatch preference.
 *
 * @param variant SIMD dispatch preference.
 * @return `auto`, `baseline`, `x86-64-v3`, `x86-64-v4`, or `sve`.
 */
std::string_view dspSimdVariantName(DspSimdVariant variant) noexcept;

/**
 * Parses an exact, case-sensitive SIMD dispatch preference.
 *
 * @param name Candidate configuration value.
 * @return Parsed preference, or no value when the name is unsupported.
 */
std::optional<DspSimdVariant>
parseDspSimdVariantName(std::string_view name) noexcept;

/**
 * Resolves a pinned SIMD preference to its concrete backend variant.
 *
 * @param variant SIMD dispatch preference.
 * @return Concrete variant, or no value for automatic dispatch.
 */
std::optional<DspBackendVariant>
concreteDspBackendVariant(DspSimdVariant variant) noexcept;

struct DspBackendAccess;

/**
 * Owns one validated native EffeTune DSP shared library.
 *
 * Instances are shared with every pipeline built from the backend. The
 * library remains loaded until all engines using it have been destroyed.
 */
class DspBackend final {
  struct Impl;

public:
  /** Closes the shared library after its last owner releases it. */
  ~DspBackend();
  /** Backends are shared by pointer and cannot be copied. */
  DspBackend(const DspBackend &) = delete;
  /** Backends cannot be copy-assigned. */
  DspBackend &operator=(const DspBackend &) = delete;

  /** Returns the validated backend variant. */
  DspBackendKind kind() const noexcept;
  /** Returns the validated concrete instruction-set variant. */
  DspBackendVariant variant() const noexcept;
  /** Returns the exact shared-library path used to load the backend. */
  const std::filesystem::path &libraryPath() const noexcept;

private:
  explicit DspBackend(std::unique_ptr<Impl> implementation);
  std::unique_ptr<Impl> implementation_;

  friend struct DspBackendAccess;
};

/**
 * Reports the result of validating and loading one DSP backend.
 */
struct DspBackendLoadResult {
  /** Loaded backend, or null when error is non-empty. */
  std::shared_ptr<const DspBackend> backend;
  /** Exact path attempted, when path resolution succeeded. */
  std::filesystem::path attemptedPath;
  /** CPU feature level required by this backend on the current architecture. */
  std::string cpuRequirement;
  /** Fatal availability or compatibility diagnostic. */
  std::string error;
  /** Concrete backend variant represented by this result. */
  DspBackendVariant variant = DspBackendVariant::scalar;
  /** True when the current CPU satisfies this variant's requirements. */
  bool cpuSupported = true;
};

/**
 * Holds the independently discovered scalar and SIMD backend variants.
 */
struct DspBackends {
  /** Mandatory compatibility backend. */
  DspBackendLoadResult scalar;
  /** Optional accelerated backend. */
  DspBackendLoadResult simd;
  /** Architecture-applicable SIMD variants in ascending capability order. */
  std::vector<DspBackendLoadResult> simdVariants;

  /**
   * Returns the load result for one backend kind.
   *
   * @param kind Backend variant.
   * @return Corresponding scalar or SIMD result.
   */
  const DspBackendLoadResult &get(DspBackendKind kind) const noexcept;

  /**
   * Finds the load result for one concrete backend variant.
   *
   * @param variant Concrete backend variant.
   * @return Matching result, or null when the variant does not apply to the
   * current architecture.
   */
  const DspBackendLoadResult *
  find(DspBackendVariant variant) const noexcept;
};

/**
 * Reports the effective backend selected from a discovered pair.
 */
struct DspBackendSelection {
  /** User-configured backend kind. */
  DspBackendKind configuredBackend;
  /** User-configured SIMD dispatch preference. */
  DspSimdVariant configuredSimdVariant = DspSimdVariant::automatic;
  /** Backend to use, or null when the mandatory scalar backend is unavailable. */
  std::shared_ptr<const DspBackend> effectiveBackend;
  /** Concrete effective backend, or no value when no backend is usable. */
  std::optional<DspBackendVariant> effectiveVariant =
      DspBackendVariant::scalar;
  /** True when a lower tier or scalar replaced the preferred SIMD tier. */
  bool fallback;
  /** Degradation diagnostic, or empty when the preferred tier is active. */
  std::string error;
};

/**
 * Resolves and validates one packaged DSP backend.
 *
 * Only executable-relative build and private installation locations are
 * inspected. Generic dynamic-loader search paths are not used.
 *
 * @param kind Backend variant to load.
 * @return Loaded backend and diagnostics.
 */
DspBackendLoadResult loadDspBackend(DspBackendKind kind);

/**
 * Resolves and validates one concrete packaged DSP backend.
 *
 * Only executable-relative build and private installation locations are
 * inspected. A backend requiring unsupported CPU instructions is rejected
 * before it is opened.
 *
 * @param variant Concrete backend variant to load.
 * @return Loaded backend and diagnostics.
 */
DspBackendLoadResult loadDspBackend(DspBackendVariant variant);

/**
 * Resolves and validates both packaged DSP backend variants.
 *
 * Scalar is always inspected because it is the required compatibility
 * fallback. Failure of either variant is retained independently.
 *
 * @return Scalar and SIMD load results.
 */
DspBackends discoverDspBackends();

/**
 * Selects an effective backend while enforcing scalar as the safe baseline.
 *
 * Configured SIMD falls back to scalar when SIMD is unavailable. If scalar is
 * unavailable, no backend is selected even when SIMD itself loaded.
 *
 * @param configuredBackend User-configured backend kind.
 * @param backends Previously discovered backend results.
 * @return Effective backend, fallback state, and diagnostic.
 */
DspBackendSelection
selectDspBackend(DspBackendKind configuredBackend,
                 const DspBackends &backends);

/**
 * Selects an effective backend using logical kind and SIMD pinning rules.
 *
 * Automatic SIMD ignores CPU-inapplicable upper tiers, but reports fallback
 * when a supported upper tier is broken. A missing pinned variant falls back
 * to scalar for startup use.
 *
 * @param configuredBackend User-configured backend kind.
 * @param configuredSimdVariant Automatic or pinned SIMD preference.
 * @param backends Previously discovered backend results.
 * @return Effective backend, concrete variant, fallback state, and diagnostic.
 */
DspBackendSelection
selectDspBackend(DspBackendKind configuredBackend,
                 DspSimdVariant configuredSimdVariant,
                 const DspBackends &backends);

} // namespace pipetune

#endif
