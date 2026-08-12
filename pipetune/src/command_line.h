#ifndef PIPETUNE_COMMAND_LINE_H
#define PIPETUNE_COMMAND_LINE_H

#include "pipetune/dsp_backend.h"
#include "pipetune/sample_rate.h"

#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace pipetune {

/**
 * Selects the top-level executable operation.
 */
enum class CommandLineAction {
  /** Load a preset and run or check the PipeWire pipeline. */
  run,
  /** Run the managed per-user daemon from optional startup configuration. */
  daemon,
  /** Bypass live and startup DSP processing. */
  bypass,
  /** Show the configured and effective sample-rate state. */
  rateGet,
  /** List automatic and fixed sample-rate choices. */
  rateList,
  /** Set and persist the sample-rate policy. */
  rateSet,
  /** List packaged DSP backend availability. */
  dspList,
  /** Show configured and effective DSP backends. */
  dspGet,
  /** Set and persist the DSP backend. */
  dspSet,
  /** Reset all startup configuration to PipeTune defaults. */
  configReset,
  /** Configure and start PipeTune for the current user. */
  setup,
  /** Stop and disable PipeTune for the current user. */
  unsetup,
  /** Ask a running PipeTune process to activate a preset. */
  loadPreset,
  /** Ask a running PipeTune process for its current status. */
  status,
  /** Print command usage. */
  help,
  /** Print the PipeTune version. */
  version
};

/**
 * Holds validated command-line settings.
 */
struct CommandLineOptions {
  /** Selected top-level operation. */
  CommandLineAction action;
  /** Formal EffeTune preset path. */
  std::filesystem::path presetPath;
  /** Explicit startup configuration path, or empty for XDG resolution. */
  std::filesystem::path configPath;
  /** Explicit control socket path, or empty for the XDG runtime default. */
  std::filesystem::path controlSocketPath;
  /** DSP and PipeWire graph-rate policy. */
  SampleRatePolicy ratePolicy;
  /** Direct-run or dspSet native backend choice. */
  DspBackendKind dspBackend;
  /** Direct-run or dspSet automatic/pinned SIMD dispatch preference. */
  DspSimdVariant dspSimdVariant;
  /** Stream channel count. */
  std::uint32_t channelCount;
  /** True to stop once both PipeWire streams are ready. */
  bool checkOnly;
  /** True to repeat setup even when the current installation is ready. */
  bool forceSetup;
  /** True to launch pipetune-gtk after setup completes. */
  bool launchGtk;
  /** True to remove app configuration during unsetup. */
  bool purge;
  /** True to perform a destructive configuration action without prompting. */
  bool assumeYes;
  /** True to print a machine-readable control response. */
  bool json;
};

/**
 * Reports parsed settings or one fatal command-line diagnostic.
 */
struct CommandLineParseResult {
  /** Parsed settings; meaningful only when error is empty. */
  CommandLineOptions options;
  /** Fatal diagnostic without a program-name prefix. */
  std::string error;
};

/**
 * Parses arguments excluding argv[0].
 *
 * @param arguments Command-line tokens.
 * @return Validated options or an error.
 */
CommandLineParseResult parseCommandLine(std::span<const std::string_view> arguments);

/**
 * Returns the complete English command usage.
 */
std::string_view commandLineUsage() noexcept;

} // namespace pipetune

#endif
