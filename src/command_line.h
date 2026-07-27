#ifndef PIPETUNE_COMMAND_LINE_H
#define PIPETUNE_COMMAND_LINE_H

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
  /** PipeWire target object, or empty for the current default sink. */
  std::string targetObject;
  /** Stable virtual sink node name. */
  std::string sinkName;
  /** Stream sample rate. */
  std::uint32_t sampleRate;
  /** Stream channel count. */
  std::uint32_t channelCount;
  /** True to stop once both PipeWire streams are ready. */
  bool checkOnly;
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
