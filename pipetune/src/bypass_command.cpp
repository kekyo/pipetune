/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "bypass_command.h"

#include "pipetune/control_protocol.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <string>
#include <utility>

namespace pipetune {

PersistentBypassResult
executePersistentBypass(const PersistentBypassOptions &options) {
  auto liveApplied = false;
  auto notice = std::string{};
  const auto exchange =
      exchangeControlMessage(options.socketPath, makeBypassControlRequest());
  if (!exchange.error.empty()) {
    notice = "running daemon is unavailable: " + exchange.error;
  } else {
    const auto response = parseControlResponse(exchange.response);
    if (!response.valid || !response.success) {
      return {.success = false,
              .liveApplied = false,
              .persistenceApplied = false,
              .notice = {},
              .error = response.error};
    }
    if (response.status.processingMode != ProcessingMode::bypass ||
        !response.status.activePreset.empty()) {
      return {.success = false,
              .liveApplied = false,
              .persistenceApplied = false,
              .notice = {},
              .error = "daemon did not confirm DSP bypass"};
    }
    liveApplied = true;
  }

  const auto persistenceError = clearStartupPreset(options.configPath);
  if (!persistenceError.empty()) {
    const auto prefix =
        liveApplied
            ? std::string("DSP bypass was applied live, but startup "
                          "persistence failed: ")
            : std::string("cannot persist startup DSP bypass: ");
    return {.success = false,
            .liveApplied = liveApplied,
            .persistenceApplied = false,
            .notice = std::move(notice),
            .error = prefix + persistenceError};
  }
  return {.success = true,
          .liveApplied = liveApplied,
          .persistenceApplied = true,
          .notice = std::move(notice),
          .error = {}};
}

} // namespace pipetune
