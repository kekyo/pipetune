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
          .outputTarget = {},
          .targetObject = {},
          .sinkName = "pipetune_sink",
          .ratePolicy = defaultSampleRatePolicy(),
          .dspBackend = DspBackendKind::scalar,
          .channelCount = 2,
          .checkOnly = false,
          .purge = false,
          .assumeYes = false,
          .json = false};
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

static CommandLineParseResult parseOutputCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  if (arguments.empty()) {
    return parseError(std::move(options),
                      "output requires list, get, set, clear, or select");
  }
  const auto operation = arguments.front();
  if (operation == "list") {
    options.action = CommandLineAction::outputList;
  } else if (operation == "get") {
    options.action = CommandLineAction::outputGet;
  } else if (operation == "set") {
    options.action = CommandLineAction::outputSet;
  } else if (operation == "clear") {
    options.action = CommandLineAction::outputClear;
  } else if (operation == "select") {
    options.action = CommandLineAction::outputSelect;
  } else {
    return parseError(std::move(options),
                      "unknown output operation: " +
                          std::string(operation));
  }

  auto sawSocket = false;
  auto sawTarget = false;
  for (auto index = std::size_t{1}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--json") {
      if (options.action != CommandLineAction::outputList &&
          options.action != CommandLineAction::outputGet) {
        return parseError(
            std::move(options),
            "--json is supported only by output list and output get");
      }
      if (options.json) {
        return parseError(std::move(options), "duplicate option: --json");
      }
      options.json = true;
      continue;
    }
    if (argument == "--socket") {
      if (sawSocket) {
        return parseError(std::move(options),
                          "duplicate option: --socket");
      }
      if (index + 1 >= arguments.size()) {
        return parseError(std::move(options),
                          "missing value for --socket");
      }
      const auto value = arguments[++index];
      if (value.empty()) {
        return parseError(std::move(options),
                          "--socket must not be empty");
      }
      sawSocket = true;
      options.controlSocketPath = value;
      continue;
    }
    if (options.action != CommandLineAction::outputSet) {
      return parseError(std::move(options),
                        "unknown output option: " +
                            std::string(argument));
    }
    if (sawTarget) {
      return parseError(std::move(options),
                        "output set accepts exactly one target");
    }
    if (argument.empty() ||
        argument.find('\n') != std::string_view::npos ||
        argument.find('\r') != std::string_view::npos ||
        argument.find('\0') != std::string_view::npos) {
      return parseError(
          std::move(options),
          "output target must be a non-empty single line without NUL");
    }
    sawTarget = true;
    options.outputTarget = argument;
  }
  if (options.action == CommandLineAction::outputSet && !sawTarget) {
    return parseError(std::move(options),
                      "output set requires a target");
  }
  return {.options = std::move(options), .error = {}};
}

static CommandLineParseResult parseRateQueryOptions(
    CommandLineOptions options,
    std::span<const std::string_view> arguments) {
  auto sawSocket = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--json") {
      if (options.json) {
        return parseError(std::move(options), "duplicate option: --json");
      }
      options.json = true;
      continue;
    }
    if (argument != "--socket") {
      return parseError(std::move(options),
                        "unknown rate option: " +
                            std::string(argument));
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

static CommandLineParseResult parseRateCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  if (arguments.empty()) {
    return parseError(std::move(options), "rate requires get, list, or set");
  }
  const auto operation = arguments.front();
  if (operation == "get") {
    options.action = CommandLineAction::rateGet;
    return parseRateQueryOptions(std::move(options), arguments.subspan(1));
  }
  if (operation == "list") {
    options.action = CommandLineAction::rateList;
    return parseRateQueryOptions(std::move(options), arguments.subspan(1));
  }
  if (operation != "set") {
    return parseError(std::move(options),
                      "unknown rate operation: " + std::string(operation));
  }
  options.action = CommandLineAction::rateSet;
  if (arguments.size() < 3) {
    return parseError(
        std::move(options),
        "rate set requires RATE and ENFORCEMENT");
  }

  const auto rate = arguments[1];
  if (rate == "max") {
    options.ratePolicy.mode = SampleRateMode::maximum;
    options.ratePolicy.fixedRate = 0;
  } else {
    auto fixedRate = std::uint32_t{0};
    if (!parseUnsigned(rate, fixedRate) ||
        !isSelectableSampleRate(fixedRate)) {
      return parseError(
          std::move(options),
          "RATE must be max, 44100, 48000, 96000, 192000, or 384000");
    }
    options.ratePolicy.mode = SampleRateMode::fixed;
    options.ratePolicy.fixedRate = fixedRate;
  }
  if (!parseSampleRateEnforcement(arguments[2],
                                  options.ratePolicy.enforcement)) {
    return parseError(std::move(options),
                      "ENFORCEMENT must be suggest or force");
  }

  auto sawSocket = false;
  for (auto index = std::size_t{3}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument != "--socket") {
      return parseError(std::move(options),
                        "unknown rate set option: " +
                            std::string(argument));
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

static CommandLineParseResult parseDspQueryOptions(
    CommandLineOptions options,
    std::span<const std::string_view> arguments) {
  auto sawSocket = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--json") {
      if (options.json) {
        return parseError(std::move(options), "duplicate option: --json");
      }
      options.json = true;
      continue;
    }
    if (argument != "--socket") {
      return parseError(std::move(options),
                        "unknown dsp option: " + std::string(argument));
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

static CommandLineParseResult parseDspCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  if (arguments.empty()) {
    return parseError(std::move(options), "dsp requires list, get, or set");
  }
  const auto operation = arguments.front();
  if (operation == "list") {
    options.action = CommandLineAction::dspList;
    return parseDspQueryOptions(std::move(options), arguments.subspan(1));
  }
  if (operation == "get") {
    options.action = CommandLineAction::dspGet;
    return parseDspQueryOptions(std::move(options), arguments.subspan(1));
  }
  if (operation != "set") {
    return parseError(std::move(options),
                      "unknown dsp operation: " + std::string(operation));
  }
  options.action = CommandLineAction::dspSet;
  if (arguments.size() < 2) {
    return parseError(std::move(options),
                      "dsp set requires scalar or simd");
  }
  const auto backend = parseDspBackendName(arguments[1]);
  if (!backend.has_value()) {
    return parseError(std::move(options),
                      "DSP backend must be scalar or simd");
  }
  options.dspBackend = *backend;

  auto sawSocket = false;
  for (auto index = std::size_t{2}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument != "--socket") {
      return parseError(std::move(options),
                        "unknown dsp set option: " +
                            std::string(argument));
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

static CommandLineParseResult parseSetupCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  options.action = CommandLineAction::setup;
  auto sawPreset = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument != "--preset") {
      return parseError(std::move(options),
                        "unknown setup option: " + std::string(argument));
    }
    if (sawPreset) {
      return parseError(std::move(options), "duplicate option: --preset");
    }
    if (index + 1 >= arguments.size()) {
      return parseError(std::move(options), "missing value for --preset");
    }
    const auto value = arguments[++index];
    if (value.empty()) {
      return parseError(std::move(options), "--preset must not be empty");
    }
    sawPreset = true;
    options.presetPath = value;
  }
  return {.options = std::move(options), .error = {}};
}

static CommandLineParseResult parseConfigCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  if (arguments.empty()) {
    return parseError(std::move(options), "config requires reset");
  }
  if (arguments.front() != "reset") {
    return parseError(
        std::move(options),
        "unknown config operation: " + std::string(arguments.front()));
  }
  options.action = CommandLineAction::configReset;
  for (const auto argument : arguments.subspan(1)) {
    if (argument != "-y" && argument != "--yes") {
      return parseError(std::move(options),
                        "unknown config reset option: " +
                            std::string(argument));
    }
    if (options.assumeYes) {
      return parseError(std::move(options),
                        "duplicate config reset confirmation option");
    }
    options.assumeYes = true;
  }
  return {.options = std::move(options), .error = {}};
}

static CommandLineParseResult parseUnsetupCommandLine(
    std::span<const std::string_view> arguments) {
  auto options = defaultOptions();
  options.action = CommandLineAction::unsetup;
  for (const auto argument : arguments) {
    if (argument != "--purge") {
      return parseError(std::move(options),
                        "unknown unsetup option: " +
                            std::string(argument));
    }
    if (options.purge) {
      return parseError(std::move(options), "duplicate option: --purge");
    }
    options.purge = true;
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
  if (!arguments.empty() && arguments.front() == "output") {
    return parseOutputCommandLine(arguments.subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "rate") {
    return parseRateCommandLine(arguments.subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "dsp") {
    return parseDspCommandLine(arguments.subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "config") {
    return parseConfigCommandLine(arguments.subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "setup") {
    return parseSetupCommandLine(arguments.subspan(1));
  }
  if (!arguments.empty() && arguments.front() == "unsetup") {
    return parseUnsetupCommandLine(arguments.subspan(1));
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
  auto sawChannels = false;
  auto sawDspBackend = false;
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
        argument != "--sink-name" && argument != "--channels" &&
        argument != "--dsp-backend") {
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
    if (argument == "--dsp-backend") {
      if (sawDspBackend) {
        return parseError(std::move(options),
                          "duplicate option: --dsp-backend");
      }
      const auto backend = parseDspBackendName(value);
      if (!backend.has_value()) {
        return parseError(std::move(options),
                          "--dsp-backend must be scalar or simd");
      }
      sawDspBackend = true;
      options.dspBackend = *backend;
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
    if (sawTarget || sawChannels || sawDspBackend || sawCheck || sawSocket) {
      return parseError(
          std::move(options),
          "only --sink-name may modify --restore-default");
    }
    options.action = CommandLineAction::restoreDefault;
    return {.options = std::move(options), .error = {}};
  }
  if (sawLoadPreset || sawStatus) {
    if (sawTarget || sawSinkName || sawChannels || sawDspBackend ||
        sawCheck) {
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
         "  pipetune output list [--json] [--socket PATH]\n"
         "  pipetune output get [--json] [--socket PATH]\n"
         "  pipetune output set TARGET [--socket PATH]\n"
         "  pipetune output clear [--socket PATH]\n"
         "  pipetune output select [--socket PATH]\n"
         "  pipetune rate get [--json] [--socket PATH]\n"
         "  pipetune rate list [--json] [--socket PATH]\n"
         "  pipetune rate set RATE ENFORCEMENT [--socket PATH]\n"
         "  pipetune dsp list [--json] [--socket PATH]\n"
         "  pipetune dsp get [--json] [--socket PATH]\n"
         "  pipetune dsp set scalar|simd [--socket PATH]\n"
         "  pipetune config reset [-y|--yes]\n"
         "  pipetune setup [--preset FILE]\n"
         "  pipetune unsetup [--purge]\n"
         "  pipetune --preset FILE [--target OBJECT] [--sink-name NAME]\n"
         "           [--channels COUNT] [--dsp-backend scalar|simd]\n"
         "           [--socket PATH] [--check]\n"
         "  pipetune --load-preset FILE [--socket PATH]\n"
         "  pipetune --status [--socket PATH]\n"
         "  pipetune --restore-default [--sink-name NAME]\n"
         "  pipetune --version\n"
         "  pipetune --help\n"
         "\n"
         "Options:\n"
         "  --config PATH    Read daemon startup configuration from PATH.\n"
         "  bypass           Disable DSP live and for future daemon starts.\n"
         "  output list      List physical outputs reported by the daemon.\n"
         "  output get       Show the preferred and effective output.\n"
         "  output set       Prefer a PipeWire output by its node.name.\n"
         "  output clear     Follow the system-default physical output.\n"
         "  output select    Choose an output from an interactive terminal.\n"
         "  rate get        Show configured and effective PCM rates.\n"
         "  rate list       List output-supported PCM rates.\n"
         "  rate set        Select max or a fixed rate and suggest or force.\n"
         "  dsp list        List scalar and SIMD backend availability.\n"
         "  dsp get         Show configured and effective DSP backends.\n"
         "  dsp set         Select scalar compatibility or SIMD acceleration.\n"
         "  config reset    Reset Bypass, output, PCM rate, and DSP backend.\n"
         "  -y, --yes       Skip the configuration reset confirmation.\n"
         "  --json           Print the complete machine-readable status.\n"
         "  setup            Enable PipeTune for the current user.\n"
         "  unsetup          Disable PipeTune for the current user.\n"
         "  --purge          Remove PipeTune app configuration during unsetup.\n"
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
         "  --channels COUNT  Use 1 through 8 planar channels (default: 2).\n"
         "  --dsp-backend BACKEND\n"
         "                    Use scalar or simd for this direct run.\n"
         "  --check           Verify stream negotiation, then exit.\n";
}

} // namespace pipetune
