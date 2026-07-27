#include "command_line.h"

#include <charconv>
#include <cstdint>
#include <string>
#include <string_view>

namespace pipetune {

static CommandLineOptions defaultOptions() {
  return {.action = CommandLineAction::run,
          .presetPath = {},
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
  for (const auto argument : arguments) {
    if (argument == "--help" || argument == "--version") {
      return parseError(std::move(options),
                        "--help and --version must be used alone");
    }
  }

  auto sawPreset = false;
  auto sawTarget = false;
  auto sawSinkName = false;
  auto sawRate = false;
  auto sawChannels = false;
  auto sawCheck = false;
  for (auto index = std::size_t{0}; index < arguments.size(); ++index) {
    const auto argument = arguments[index];
    if (argument == "--check") {
      if (sawCheck) {
        return parseError(std::move(options), "duplicate option: --check");
      }
      sawCheck = true;
      options.checkOnly = true;
      continue;
    }

    if (argument != "--preset" && argument != "--target" &&
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

  if (!sawPreset) {
    return parseError(std::move(options), "--preset FILE is required");
  }
  return {.options = std::move(options), .error = {}};
}

std::string_view commandLineUsage() noexcept {
  return "Usage:\n"
         "  pipetune --preset FILE [--target OBJECT] [--sink-name NAME]\n"
         "           [--rate HZ] [--channels COUNT] [--check]\n"
         "  pipetune --version\n"
         "  pipetune --help\n"
         "\n"
         "Options:\n"
         "  --preset FILE     Load a formal .effetune_preset file.\n"
         "  --target OBJECT   Send processed audio to this PipeWire sink.\n"
         "                    The current default sink is used when omitted.\n"
         "  --sink-name NAME  Publish this virtual sink name (default: "
         "pipetune_sink).\n"
         "  --rate HZ         Use 32000 through 192000 Hz (default: 48000).\n"
         "  --channels COUNT  Use 1 through 8 planar channels (default: 2).\n"
         "  --check           Verify stream negotiation, then exit.\n";
}

} // namespace pipetune
