#include "command_line.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace pipetune {

static CommandLineOptions defaultOptions() {
  return {.action = CommandLineAction::run,
          .presetPath = {},
          .configPath = {},
          .controlSocketPath = {},
          .targetObject = {},
          .sinkName = "pipetune_sink",
          .sampleRate = 48000,
          .channelCount = 2,
          .checkOnly = false};
}

static CommandLineParseResult parseError(CommandLineOptions options,
                                         std::string message) {
  return {.options = std::move(options), .error = std::move(message)};
}

static bool parseUnsigned(std::string_view text, std::uint32_t &output) {
  if (text.empty()) {
    return false;
  }
  auto value = std::uint32_t{0};
  const auto result =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
    return false;
  }
  output = value;
  return true;
}

static CommandLineParseResult parseDaemonCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  options.action = CommandLineAction::daemon;
  auto sawConfig = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument != "--config") {
      return parseError(std::move(options),
                        "unknown daemon option: " + std::string(argument));
    }
    if (sawConfig) {
      return parseError(std::move(options), "duplicate option: --config");
    }
    if (index + 1 >= arguments.size()) {
      return parseError(std::move(options), "missing value for --config");
    }
    const auto value = arguments[++index];
    if (value.empty()) {
      return parseError(std::move(options), "--config must not be empty");
    }
    sawConfig = true;
    options.configPath = value;
  }
  return {.options = std::move(options), .error = {}};
}

static CommandLineParseResult parseBypassCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  options.action = CommandLineAction::bypass;
  auto sawSocket = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument != "--socket") {
      return parseError(std::move(options),
                        "unknown bypass option: " + std::string(argument));
    }
    if (sawSocket) {
      return parseError(std::move(options), "duplicate option: --socket");
    }
    if (index + 1 >= arguments.size()) {
      return parseError(std::move(options), "missing value for --socket");
    }
    const auto value = arguments[++index];
    if (value.empty()) {
      return parseError(std::move(options), "--socket must not be empty");
    }
    sawSocket = true;
    options.controlSocketPath = value;
  }
  return {.options = std::move(options), .error = {}};
}

CommandLineParseResult parseCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  if (arguments.size() == 1 && arguments[0] == "--help") {
    options.action = CommandLineAction::help;
    return {.options = std::move(options), .error = {}};
  }
  if (arguments.size() == 1 && arguments[0] == "--version") {
    options.action = CommandLineAction::version;
    return {.options = std::move(options), .error = {}};
  }
  if (!arguments.empty() && arguments.front() == "daemon") {
    return parseDaemonCommandLine(arguments.subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "bypass") {
    return parseBypassCommandLine(arguments.subspan(1));
  }
  for (const auto argument : arguments) {
    if (argument == "--help" || argument == "--version") {
      return parseError(std::move(options),
                        "--help and --version must be used alone");
    }
  }

  auto sawPreset = false;
  auto sawLoadPreset = false;
  auto sawStatus = false;
  auto sawRestoreDefault = false;
  auto sawSocket = false;
  auto sawTarget = false;
  auto sawSinkName = false;
  auto sawRate = false;
  auto sawChannels = false;
  auto sawCheck = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--status") {
      if (sawStatus) {
        return parseError(std::move(options), "duplicate option: --status");
      }
      sawStatus = true;
      continue;
    }
    if (argument == "--restore-default") {
      if (sawRestoreDefault) {
        return parseError(std::move(options),
                          "duplicate option: --restore-default");
      }
      sawRestoreDefault = true;
      continue;
    }
    if (argument == "--check") {
      if (sawCheck) {
        return parseError(std::move(options), "duplicate option: --check");
      }
      sawCheck = true;
      options.checkOnly = true;
      continue;
    }

    if (argument != "--preset" && argument != "--load-preset" &&
        argument != "--socket" && argument != "--target" &&
        argument != "--sink-name" && argument != "--rate" &&
        argument != "--channels") {
      return parseError(std::move(options),
                        "unknown option: " + std::string(argument));
    }
    if (index + 1 >= arguments.size()) {
      return parseError(std::move(options),
                        "missing value for " + std::string(argument));
    }
    const auto value = arguments[++index];

    if (argument == "--load-preset") {
      if (sawLoadPreset) {
        return parseError(std::move(options),
                          "duplicate option: --load-preset");
      }
      if (value.empty()) {
        return parseError(std::move(options),
                          "--load-preset must not be empty");
      }
      sawLoadPreset = true;
      options.presetPath = std::string(value);
      continue;
    }
    if (argument == "--preset") {
      if (sawPreset) {
        return parseError(std::move(options), "duplicate option: --preset");
      }
      if (value.empty()) {
        return parseError(std::move(options), "--preset must not be empty");
      }
      sawPreset = true;
      options.presetPath = std::string(value);
      continue;
    }
    if (argument == "--socket") {
      if (sawSocket) {
        return parseError(std::move(options), "duplicate option: --socket");
      }
      if (value.empty()) {
        return parseError(std::move(options), "--socket must not be empty");
      }
      sawSocket = true;
      options.controlSocketPath = std::string(value);
      continue;
    }
    if (argument == "--target") {
      if (sawTarget) {
        return parseError(std::move(options), "duplicate option: --target");
      }
      sawTarget = true;
      options.targetObject = value;
      continue;
    }
    if (argument == "--sink-name") {
      if (sawSinkName) {
        return parseError(std::move(options), "duplicate option: --sink-name");
      }
      if (value.empty()) {
        return parseError(std::move(options), "--sink-name must not be empty");
      }
      sawSinkName = true;
      options.sinkName = value;
      continue;
    }
    if (argument == "--rate") {
      if (sawRate) {
        return parseError(std::move(options), "duplicate option: --rate");
      }
      auto rate = std::uint32_t{0};
      if (!parseUnsigned(value, rate) || rate < 32000 || rate > 192000) {
        return parseError(std::move(options),
                          "--rate must be an integer from 32000 through 192000");
      }
      sawRate = true;
      options.sampleRate = rate;
      continue;
    }

    if (sawChannels) {
      return parseError(std::move(options), "duplicate option: --channels");
    }
    auto channels = std::uint32_t{0};
    if (!parseUnsigned(value, channels) || channels == 0 || channels > 8) {
      return parseError(std::move(options),
                        "--channels must be an integer from 1 through 8");
    }
    sawChannels = true;
    options.channelCount = channels;
  }

  const auto actionCount = static_cast<unsigned>(sawPreset) +
                           static_cast<unsigned>(sawLoadPreset) +
                           static_cast<unsigned>(sawStatus) +
                           static_cast<unsigned>(sawRestoreDefault);
  if (actionCount > 1) {
    return parseError(
        std::move(options),
        "top-level action options are mutually exclusive");
  }
  if (sawRestoreDefault) {
    if (sawTarget || sawRate || sawChannels || sawCheck || sawSocket) {
      return parseError(
          std::move(options),
          "only --sink-name may modify --restore-default");
    }
    options.action = CommandLineAction::restoreDefault;
    return {.options = std::move(options), .error = {}};
  }
  if (sawLoadPreset || sawStatus) {
    if (sawTarget || sawSinkName || sawRate || sawChannels || sawCheck) {
      return parseError(
          std::move(options),
          "PipeWire run options cannot be used with control actions");
    }
    options.action = sawLoadPreset ? CommandLineAction::loadPreset
                                   : CommandLineAction::status;
    return {.options = std::move(options), .error = {}};
  }
  if (!sawPreset) {
    return parseError(std::move(options), "--preset FILE is required");
  }
  return {.options = std::move(options), .error = {}};
}

std::string_view commandLineUsage() noexcept {
  return "Usage:\n"
         "  pipetune daemon [--config PATH]\n"
         "  pipetune bypass [--socket PATH]\n"
         "  pipetune --preset FILE [--target OBJECT] [--sink-name NAME]\n"
         "           [--rate HZ] [--channels COUNT] [--socket PATH] [--check]\n"
         "  pipetune --load-preset FILE [--socket PATH]\n"
         "  pipetune --status [--socket PATH]\n"
         "  pipetune --restore-default [--sink-name NAME]\n"
         "  pipetune --version\n"
         "  pipetune --help\n"
         "\n"
         "Options:\n"
         "  --config PATH    Read daemon startup configuration from PATH.\n"
         "  bypass           Disable DSP live and for future daemon starts.\n"
         "  --preset FILE     Load a formal .effetune_preset file.\n"
         "  --load-preset FILE\n"
         "                    Replace the preset in a running PipeTune process.\n"
         "  --status          Print the running PipeTune process status as JSON.\n"
         "  --restore-default\n"
         "                    Restore an available physical PipeWire sink.\n"
         "  --socket PATH     Use this control socket instead of the XDG default.\n"
         "  --target OBJECT   Send processed audio to this PipeWire sink.\n"
         "                    The current default sink is used when omitted.\n"
         "  --sink-name NAME  Publish this virtual sink name (default: "
         "pipetune_sink).\n"
         "  --rate HZ         Use 32000 through 192000 Hz (default: 48000).\n"
         "  --channels COUNT  Use 1 through 8 planar channels (default: 2).\n"
         "  --check           Verify stream negotiation, then exit.\n";
}

} // namespace pipetune
