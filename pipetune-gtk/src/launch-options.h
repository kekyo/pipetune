#ifndef PIPETUNE_GTK_LAUNCH_OPTIONS_H
#define PIPETUNE_GTK_LAUNCH_OPTIONS_H

#include <span>
#include <string>
#include <string_view>

namespace pipetune_gtk {

/**
 * Selects the command-line action.
 */
enum class LaunchAction {
  /** Run or activate the tray application. */
  run,
  /** Print command-line help. */
  help,
  /** Print the application version. */
  version
};

/**
 * Stores validated command-line options.
 */
struct LaunchOptions {
  /** Requested command-line action. */
  LaunchAction action;
  /** True when the initial window should stay hidden. */
  bool hidden;
};

/**
 * Reports parsed launch options or one diagnostic.
 */
struct LaunchOptionsParseResult {
  /** Parsed options; meaningful when error is empty. */
  LaunchOptions options;
  /** Validation diagnostic, or empty on success. */
  std::string error;
};

/**
 * Parses arguments excluding argv[0].
 *
 * @param arguments Command-line arguments.
 * @return Parsed options or diagnostic.
 */
LaunchOptionsParseResult
parseLaunchOptions(std::span<const std::string_view> arguments);

/**
 * Returns command-line usage text.
 *
 * @return Static usage text.
 */
std::string_view launchOptionsUsage();

} // namespace pipetune_gtk

#endif
