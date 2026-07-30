#ifndef PIPETUNE_DSP_BACKEND_H
#define PIPETUNE_DSP_BACKEND_H

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

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
};

/**
 * Holds the independently discovered scalar and SIMD backend variants.
 */
struct DspBackends {
  /** Mandatory compatibility backend. */
  DspBackendLoadResult scalar;
  /** Optional accelerated backend. */
  DspBackendLoadResult simd;

  /**
   * Returns the load result for one backend kind.
   *
   * @param kind Backend variant.
   * @return Corresponding scalar or SIMD result.
   */
  const DspBackendLoadResult &get(DspBackendKind kind) const noexcept;
};

/**
 * Reports the effective backend selected from a discovered pair.
 */
struct DspBackendSelection {
  /** User-configured backend kind. */
  DspBackendKind configuredBackend;
  /** Backend to use, or null when the mandatory scalar backend is unavailable. */
  std::shared_ptr<const DspBackend> effectiveBackend;
  /** True when configured SIMD could not be used and scalar was selected. */
  bool fallback;
  /** Availability diagnostic, or empty when the configured backend is active. */
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

} // namespace pipetune

#endif
