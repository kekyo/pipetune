#include "launch-options.h"

#include <array>
#include <iostream>
#include <span>
#include <string_view>

static bool check(bool condition, std::string_view message) {
  if (!condition) {
    std::cerr << message << '\n';
  }
  return condition;
}

int main() {
  const auto defaults = pipetune_gtk::parseLaunchOptions({});
  const auto hiddenArguments =
      std::array<std::string_view, 1>{"--hidden"};
  const auto hidden = pipetune_gtk::parseLaunchOptions(hiddenArguments);
  const auto helpArguments = std::array<std::string_view, 1>{"--help"};
  const auto help = pipetune_gtk::parseLaunchOptions(helpArguments);
  const auto versionArguments =
      std::array<std::string_view, 1>{"--version"};
  const auto version = pipetune_gtk::parseLaunchOptions(versionArguments);
  const auto invalidArguments =
      std::array<std::string_view, 1>{"unexpected"};
  const auto invalid = pipetune_gtk::parseLaunchOptions(invalidArguments);

  return check(defaults.error.empty() && !defaults.options.hidden &&
                   defaults.options.action ==
                       pipetune_gtk::LaunchAction::run,
               "default launch options differ") &&
                 check(hidden.error.empty() && hidden.options.hidden,
                       "hidden launch option differs") &&
                 check(help.error.empty() &&
                           help.options.action ==
                               pipetune_gtk::LaunchAction::help,
                       "help launch option differs") &&
                 check(version.error.empty() &&
                           version.options.action ==
                               pipetune_gtk::LaunchAction::version,
                       "version launch option differs") &&
                 check(!invalid.error.empty(),
                       "positional arguments must be rejected") &&
                 check(pipetune_gtk::launchOptionsUsage().find(
                           "--hidden") != std::string_view::npos,
                       "usage must document hidden startup")
             ? 0
             : 1;
}
