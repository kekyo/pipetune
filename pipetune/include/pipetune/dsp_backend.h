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
 * Resolves and validates one packaged DSP backend.
 *
 * Only executable-relative build and private installation locations are
 * inspected. Generic dynamic-loader search paths are not used.
 *
 * @param kind Backend variant to load.
 * @return Loaded backend and diagnostics.
 */
DspBackendLoadResult loadDspBackend(DspBackendKind kind);

} // namespace pipetune

#endif
