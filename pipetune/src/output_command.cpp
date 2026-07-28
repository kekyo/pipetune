#include "output_command.h"

#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <charconv>
#include <cstddef>
#include <istream>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace pipetune {

static OutputStatusQueryResult outputStatusError(std::string error) {
  return {.success = false,
          .status = {},
          .json = {},
          .error = std::move(error)};
}

OutputStatusQueryResult
queryOutputStatus(const std::filesystem::path &socketPath) {
  const auto exchange =
      exchangeControlMessage(socketPath, makeStatusControlRequest());
  if (!exchange.error.empty()) {
    return outputStatusError(exchange.error);
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid || !response.success) {
    return outputStatusError(std::move(response.error));
  }
  if (response.kind != ControlResponseKind::response) {
    return outputStatusError("daemon returned an unexpected status event");
  }
  return {.success = true,
          .status = std::move(response.status),
          .json = exchange.response,
          .error = {}};
}

static PersistentOutputResult outputChangeError(std::string error) {
  return {.success = false,
          .liveApplied = false,
          .persistenceApplied = false,
          .status = {},
          .error = std::move(error)};
}

static bool outputTargetIsValid(std::string_view target) {
  return !target.empty() && target.find('\0') == std::string_view::npos &&
         target.find('\n') == std::string_view::npos &&
         target.find('\r') == std::string_view::npos;
}

static PersistentOutputResult executeOutputChange(
    const PersistentOutputOptions &options, std::string_view target,
    bool clearPreference) {
  if (!clearPreference && !outputTargetIsValid(target)) {
    return outputChangeError(
        "output target must be a non-empty single line without NUL");
  }
  const auto request =
      clearPreference ? makeClearOutputControlRequest()
                      : makeSetOutputControlRequest(target);
  if (request.empty()) {
    return outputChangeError("cannot encode output-change request");
  }
  const auto exchange =
      exchangeControlMessage(options.socketPath, request);
  if (!exchange.error.empty()) {
    return outputChangeError(exchange.error);
  }
  auto response = parseControlResponse(exchange.response);
  if (!response.valid || !response.success) {
    return outputChangeError(std::move(response.error));
  }
  if (response.kind != ControlResponseKind::response) {
    return outputChangeError(
        "daemon returned an unexpected output status event");
  }
  if ((clearPreference && !response.status.preferredTarget.empty()) ||
      (!clearPreference && response.status.preferredTarget != target)) {
    return outputChangeError(
        "daemon did not confirm the requested output preference");
  }

  const auto persistenceError =
      clearPreference
          ? clearPreferredOutput(options.configPath)
          : savePreferredOutput(options.configPath, target);
  if (!persistenceError.empty()) {
    return {.success = false,
            .liveApplied = true,
            .persistenceApplied = false,
            .status = std::move(response.status),
            .error =
                "output preference was applied live, but startup "
                "persistence failed: " +
                persistenceError};
  }
  return {.success = true,
          .liveApplied = true,
          .persistenceApplied = true,
          .status = std::move(response.status),
          .error = {}};
}

PersistentOutputResult
executeSetPreferredOutput(const PersistentOutputOptions &options,
                          std::string_view target) {
  return executeOutputChange(options, target, false);
}

PersistentOutputResult
executeClearPreferredOutput(const PersistentOutputOptions &options) {
  return executeOutputChange(options, {}, true);
}

static std::string_view outputReasonLabel(
    ControlOutputSelectionReason reason) {
  switch (reason) {
  case ControlOutputSelectionReason::unavailable:
    return "Unavailable because no audio output exists";
  case ControlOutputSelectionReason::systemDefault:
    return "System default";
  case ControlOutputSelectionReason::preferred:
    return "Explicit preference";
  case ControlOutputSelectionReason::fallback:
    return "Fallback because the preferred output is unavailable";
  }
  return "Unknown";
}

static const ControlOutputDevice *findOutput(
    const ControlRuntimeStatus &status, std::string_view name) {
  for (const auto &output : status.availableOutputs) {
    if (output.name == name) {
      return &output;
    }
  }
  return nullptr;
}

std::string formatOutputDeviceList(const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  if (status.availableOutputs.empty()) {
    formatted << "No audio output devices are currently available.\n";
    return formatted.str();
  }
  formatted << "Available audio outputs:\n";
  for (auto index = std::size_t{0};
       index < status.availableOutputs.size(); ++index) {
    const auto &output = status.availableOutputs[index];
    formatted << "  " << index + 1 << ". " << output.description << '\n'
              << "     name: " << output.name << '\n';
    if (output.systemDefault || output.preferred || output.selected) {
      formatted << "     status:";
      if (output.systemDefault) {
        formatted << " system default";
      }
      if (output.preferred) {
        formatted << (output.systemDefault ? "," : "") << " preferred";
      }
      if (output.selected) {
        formatted << (output.systemDefault || output.preferred ? "," : "")
                  << " selected";
      }
      formatted << '\n';
    }
  }
  return formatted.str();
}

std::string formatOutputSelection(const ControlRuntimeStatus &status) {
  auto formatted = std::ostringstream{};
  formatted << "Preferred output: ";
  if (status.preferredTarget.empty()) {
    formatted << "System default (no explicit preference)\n";
  } else {
    const auto *preferred =
        findOutput(status, status.preferredTarget);
    if (preferred == nullptr) {
      formatted << status.preferredTarget
                << " (currently unavailable)\n";
    } else {
      formatted << preferred->description << " ("
                << preferred->name << ")\n";
    }
  }

  formatted << "Selected output: ";
  if (status.selectedTarget.empty()) {
    formatted << "Unavailable\n";
  } else {
    const auto *selected =
        findOutput(status, status.selectedTarget);
    if (selected == nullptr) {
      formatted << status.selectedTarget << '\n';
    } else {
      formatted << selected->description << " ("
                << selected->name << ")\n";
    }
  }
  formatted << "Selection reason: "
            << outputReasonLabel(status.outputSelectionReason) << '\n';
  return formatted.str();
}

static bool parseSelection(std::string_view line, std::size_t &selection) {
  const auto first = line.find_first_not_of(" \t");
  if (first == std::string_view::npos) {
    return false;
  }
  const auto last = line.find_last_not_of(" \t");
  const auto text = line.substr(first, last - first + 1);
  auto value = std::size_t{0};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size()) {
    return false;
  }
  selection = value;
  return true;
}

OutputSelectionChoice
promptForOutputSelection(const ControlRuntimeStatus &status,
                         std::istream &input, std::ostream &output) {
  output << "Choose an audio output:\n"
         << "  0. System default (clear explicit preference)\n";
  for (auto index = std::size_t{0};
       index < status.availableOutputs.size(); ++index) {
    const auto &device = status.availableOutputs[index];
    output << "  " << index + 1 << ". " << device.description
           << " (" << device.name << ')';
    if (device.preferred) {
      output << " [preferred]";
    }
    if (device.selected) {
      output << " [selected]";
    }
    output << '\n';
  }
  if (!status.preferredTarget.empty() &&
      findOutput(status, status.preferredTarget) == nullptr) {
    output << "Current preference is unavailable: "
           << status.preferredTarget << '\n';
  }

  auto line = std::string{};
  while (true) {
    output << "Selection: " << std::flush;
    if (!std::getline(input, line)) {
      return {.success = false,
              .clearPreference = false,
              .target = {},
              .error = "output selection was cancelled"};
    }
    auto selection = std::size_t{0};
    if (!parseSelection(line, selection) ||
        selection > status.availableOutputs.size()) {
      output << "Invalid selection. Enter 0 through "
             << status.availableOutputs.size() << ".\n";
      continue;
    }
    if (selection == 0) {
      return {.success = true,
              .clearPreference = true,
              .target = {},
              .error = {}};
    }
    return {.success = true,
            .clearPreference = false,
            .target = status.availableOutputs[selection - 1].name,
            .error = {}};
  }
}

} // namespace pipetune
