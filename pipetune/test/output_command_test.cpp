/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "output_command.h"
#include "pipetune/control_socket.h"
#include "pipetune/startup_config.h"

#include <array>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <unistd.h>

static bool check(bool condition, std::string_view message) {
  if (!condition) std::cerr << message << '\n';
  return condition;
}

static pipetune::AvailableOutput device(std::string name, unsigned id) {
  return {.device = {.identity = {.api = "node", .location = name, .port = name,
      .vendor = {}, .product = {}, .serial = {}}, .name = "DAC " + name, .profile = "stereo",
      .channelPositions = {"FL", "FR"}}, .nodeName = name, .nodeId = id, .nodeSerial = id};
}

static bool testSelectionAndDisplay() {
  const auto inventory = std::array{device("a", 1), device("b", 2), device("c", 3)};
  const auto initial = pipetune::selectOutputDevices({}, inventory, std::array<std::string, 2>{"a", "b"});
  if (!check(initial.error.empty(), initial.error) ||
      !check(initial.configuration.mode == pipetune::OutputMode::multiple && initial.configuration.channels.size() == 4,
             "selecting two stereo outputs must allocate four numbered slots")) return false;
  auto labeled = initial.configuration;
  labeled.channels[2].label = "サブ左";
  const auto changed = pipetune::selectOutputDevices(labeled, inventory, std::array<std::string, 2>{"b", "c"});
  if (!check(changed.error.empty(), changed.error) ||
      !check(!changed.configuration.outputs[0].enabled && changed.configuration.outputs[1].enabled &&
             changed.configuration.channels.size() == 6 && changed.configuration.channels[2] == labeled.channels[2],
             "selection must reserve disabled slots and retain existing channel numbers and labels")) return false;
  const auto restored = pipetune::selectOutputDevices(changed.configuration, inventory, std::array<std::string, 2>{"b", "a"});
  if (!check(restored.error.empty() && restored.configuration.channels == changed.configuration.channels &&
             restored.configuration.outputs[0].enabled && !restored.configuration.outputs[2].enabled,
             "reselection must not allocate or renumber old slots")) return false;
  for (const auto &names : {std::vector<std::string>{}, {"a", "a"}, {"missing"}}) {
    const auto invalid = pipetune::selectOutputDevices(labeled, inventory, names);
    if (!check(!invalid.error.empty() && invalid.configuration == labeled, "invalid selections must retain the previous configuration")) return false;
  }
  auto changedProfile = inventory;
  changedProfile[0].device.profile = "surround";
  auto ambiguous = std::vector<pipetune::AvailableOutput>{inventory[0], inventory[0]};
  ambiguous[1].nodeName = "duplicate";
  if (!check(!pipetune::selectOutputDevices(labeled, changedProfile, std::array<std::string, 1>{"a"}).error.empty(),
             "changed profiles require explicit reassignment") ||
      !check(!pipetune::selectOutputDevices({}, ambiguous, std::array<std::string, 1>{"a"}).error.empty(),
             "ambiguous physical identity must not be accepted as a selectable target")) return false;
  auto status = pipetune::ControlRuntimeStatus{};
  status.outputConfiguration = changed.configuration;
  status.availableOutputs = {inventory[0], inventory[2]};
  status.outputInventoryReady = true;
  const auto display = pipetune::formatOutputStatus(status);
  if (!check(display.find("Ch 3 | DAC b | 1 (FL)") != std::string::npos &&
             display.find("サブ左") != std::string::npos && display.find("missing") != std::string::npos &&
             display.find("disabled") != std::string::npos && display.find("connected") != std::string::npos,
             "the table must show fixed EffeTune numbers, device channels, labels, and actual presence")) return false;
  status.outputConfiguration.mode = pipetune::OutputMode::single;
  return check(pipetune::formatOutputStatus(status).find("OS selects") != std::string::npos &&
               pipetune::formatOutputStatus(status).find("inactive") != std::string::npos,
               "single mode must distinguish inactive saved assignments from OS-selected output");
}

struct ServerState {
  std::mutex mutex;
  pipetune::ControlRuntimeStatus status = {};
  std::filesystem::path configPath;
  bool reject = false;
  bool breakSave = false;
  bool rejectRollback = false;
  bool invalidReply = false;
  unsigned setCount = 0;
};

static pipetune::ControlMessageResult handle(std::string_view json, void *data) {
  auto &state = *static_cast<ServerState *>(data);
  auto lock = std::scoped_lock(state.mutex);
  const auto request = pipetune::parseControlRequest(json);
  auto error = request.error;
  if (error.empty() && request.request.command == pipetune::ControlCommand::setOutput) {
    if (state.reject || (state.rejectRollback && state.setCount != 0)) error = "output change rejected";
    else if (request.request.expectedRevision != state.status.configurationRevision) error = "configuration changed";
    else {
      state.status.outputConfiguration = request.request.outputConfiguration;
      ++state.status.configurationRevision;
      ++state.setCount;
      if (state.breakSave && state.setCount == 1) {
        const auto parent = state.configPath.parent_path();
        std::filesystem::rename(parent, parent.string() + ".retained");
        std::ofstream(parent) << "block atomic persistence";
      }
    }
  }
  return {.response = !error.empty() ? pipetune::makeControlErrorResponse(error) :
      state.invalidReply && request.request.command == pipetune::ControlCommand::setOutput ? "{}" :
      pipetune::makeControlSuccessResponse(state.status, {}),
      .connectionMode = pipetune::ControlConnectionMode::close, .publishStatus = false};
}

static bool testPersistence(const std::filesystem::path &directory) {
  const auto configPath = directory / "settings" / "environment";
  const auto socketPath = directory / "control.sock";
  auto initial = pipetune::StartupConfig{};
  initial.presetFound = true;
  initial.presetPath = directory / "preset.effetune_preset";
  initial.dspIdlePolicy.timeoutMilliseconds = 100;
  if (!check(pipetune::saveStartupConfig(configPath, initial).empty(), "cannot save fixture")) return false;
  auto state = ServerState{};
  state.configPath = configPath;
  state.status.availableOutputs = {device("a", 1), device("b", 2)};
  state.status.outputInventoryReady = true;
  auto server = pipetune::startControlServer(socketPath, {.handler = handle, .statusProvider = nullptr, .userData = &state});
  if (!check(server.server != nullptr, server.error)) return false;
  const auto selected = pipetune::executeOutputChange({configPath, socketPath},
      {.kind = pipetune::OutputChangeKind::select, .nodes = {"a", "b"}});
  auto saved = pipetune::loadStartupConfig(configPath);
  if (!check(selected.success && selected.liveApplied && selected.persistenceApplied, selected.error) ||
      !check(saved.error.empty() && saved.config.outputConfiguration == selected.status.outputConfiguration &&
             saved.config.presetPath == initial.presetPath && saved.config.dspIdlePolicy == initial.dspIdlePolicy,
             "saving outputs must preserve other startup choices")) return false;
  {
    auto lock = std::scoped_lock(state.mutex);
    state.reject = true;
  }
  const auto rejected = pipetune::executeOutputChange({configPath, socketPath},
      {.kind = pipetune::OutputChangeKind::mode, .configuration = {.mode = pipetune::OutputMode::single, .outputs = {}, .channels = {}}});
  if (!check(!rejected.success && !rejected.persistenceApplied &&
             pipetune::loadStartupConfig(configPath).config.outputConfiguration == saved.config.outputConfiguration,
             "daemon rejection must leave saved configuration intact")) return false;
  for (const auto rejectRollback : {false, true}) {
    {
      auto lock = std::scoped_lock(state.mutex);
      state.status.outputConfiguration = selected.status.outputConfiguration;
      state.reject = false;
      state.breakSave = true;
      state.rejectRollback = rejectRollback;
      state.setCount = 0;
    }
    const auto failed = pipetune::executeOutputChange({configPath, socketPath},
        {.kind = pipetune::OutputChangeKind::mode, .configuration = {.mode = pipetune::OutputMode::single, .outputs = {}, .channels = {}}});
    const auto parent = configPath.parent_path();
    if (std::filesystem::exists(parent.string() + ".retained")) {
      std::filesystem::remove(parent);
      std::filesystem::rename(parent.string() + ".retained", parent);
    }
    if (!check(!failed.success && !failed.persistenceApplied && failed.rollbackApplied == !rejectRollback,
               "save failure must restore old live routing or explicitly report failed restoration") ||
        !check(pipetune::loadStartupConfig(configPath).config.outputConfiguration == saved.config.outputConfiguration,
               "failed saves must preserve the old file")) return false;
    const auto live = pipetune::queryOutputStatus(socketPath);
    if (!check(live.error.empty() && live.status.outputConfiguration.mode ==
               (rejectRollback ? pipetune::OutputMode::single : pipetune::OutputMode::multiple),
               "reported restoration must agree with the daemon state")) return false;
  }
  {
    auto lock = std::scoped_lock(state.mutex);
    state.rejectRollback = false;
    state.breakSave = false;
    state.invalidReply = true;
  }
  const auto uncertain = pipetune::executeOutputChange({configPath, socketPath},
      {.kind = pipetune::OutputChangeKind::mode, .configuration = {.mode = pipetune::OutputMode::multiple, .outputs = {}, .channels = {}}});
  if (!check(!uncertain.success && !uncertain.persistenceApplied &&
             pipetune::loadStartupConfig(configPath).config.outputConfiguration == saved.config.outputConfiguration,
             "an invalid live reply must not become offline persistence success")) return false;
  server.server.reset();
  const auto unavailable = pipetune::exchangeControlMessage(socketPath, pipetune::makeStatusControlRequest());
  if (!check(unavailable.unavailable && !unavailable.error.empty(), "an absent daemon must be distinguishable from uncertain delivery")) return false;
  const auto offline = pipetune::executeOutputChange({configPath, socketPath},
      {.kind = pipetune::OutputChangeKind::mode, .configuration = {.mode = pipetune::OutputMode::single, .outputs = {}, .channels = {}}});
  saved = pipetune::loadStartupConfig(configPath);
  return check(offline.success && !offline.liveApplied && offline.persistenceApplied && !offline.notice.empty() &&
               saved.error.empty() && saved.config.outputConfiguration.mode == pipetune::OutputMode::single &&
               saved.config.outputConfiguration.channels == selected.status.outputConfiguration.channels,
               "offline mode changes must preserve slots and explain deferred application");
}

int main() {
  const auto directory = std::filesystem::temp_directory_path() / ("pipetune-output-command-" + std::to_string(getpid()));
  std::filesystem::create_directories(directory);
  const auto selection = testSelectionAndDisplay();
  const auto persistence = testPersistence(directory);
  std::filesystem::remove_all(directory);
  return selection && persistence ? 0 : 1;
}
