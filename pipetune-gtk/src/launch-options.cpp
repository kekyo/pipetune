#include "launch-options.h"

namespace pipetune_gtk {

LaunchOptionsParseResult
parseLaunchOptions(std::span<const std::string_view> arguments) {
  auto options =
      LaunchOptions{.action = LaunchAction::run, .hidden = false};
  for (const auto argument : arguments) {
    if (argument == "--hidden") {
      if (options.action != LaunchAction::run || options.hidden) {
        return {.options = options,
                .error = "launch options must not be repeated or combined"};
      }
      options.hidden = true;
      continue;
    }
    if (argument == "--help" || argument == "-h") {
      if (options.action != LaunchAction::run || options.hidden) {
        return {.options = options,
                .error = "launch options must not be repeated or combined"};
      }
      options.action = LaunchAction::help;
      continue;
    }
    if (argument == "--version") {
      if (options.action != LaunchAction::run || options.hidden) {
        return {.options = options,
                .error = "launch options must not be repeated or combined"};
      }
      options.action = LaunchAction::version;
      continue;
    }
    if (argument == "--quit") {
      if (options.action != LaunchAction::run || options.hidden) {
        return {.options = options,
                .error = "launch options must not be repeated or combined"};
      }
      options.action = LaunchAction::quit;
      continue;
    }
    return {.options = options,
            .error = "unknown PipeTune GTK option: " +
                     std::string(argument)};
  }
  return {.options = options, .error = {}};
}

std::string_view launchOptionsUsage() {
  return "Usage: pipetune-gtk [--hidden]\n"
         "       pipetune-gtk --quit\n"
         "       pipetune-gtk --help\n"
         "       pipetune-gtk --version\n\n"
         "Options:\n"
         "  --hidden   Start in the system tray without showing the window.\n"
         "  --quit     Quit the running PipeTune GTK application.\n"
         "  --help     Show this help text.\n"
         "  --version  Show the PipeTune GTK version.\n";
}

} // namespace pipetune_gtk
