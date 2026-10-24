/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_inventory.h"

#include <pipewire/pipewire.h>

#include <algorithm>
#include <iostream>

struct State {
  pw_main_loop *loop = nullptr;
  pipetune::PipeWireOutputInventory *observer = nullptr;
  std::string error;
  bool complete = false;
  unsigned step = 0;
  std::vector<pipetune::PipeWireInputRouteSnapshot> originalInputRoutes;
  pipetune::OutputCardIdentity card;
};

static void changed(const pipetune::OutputInventoryResult &snapshot, void *data) {
  auto &state = *static_cast<State *>(data);
  if (!snapshot.error.empty()) {
    state.error = snapshot.error;
    pw_main_loop_quit(state.loop);
    return;
  }
  const auto first = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &profile) {
    return profile.card.api == "pipetune-fixture" &&
        profile.cardProfileName == "output:analog-stereo+input:analog-stereo";
  });
  const auto second = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &profile) {
    return profile.card.api == "pipetune-fixture" &&
        profile.cardProfileName == "output:iec958-stereo+input:analog-stereo";
  });
  const auto inputChanging = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &profile) {
    return profile.card.api == "pipetune-fixture" &&
        profile.cardProfileName == "output:analog-stereo+input:other";
  });
  const auto inputOnly = std::find_if(snapshot.profiles.begin(), snapshot.profiles.end(), [](const auto &profile) {
    return profile.card.api == "pipetune-fixture" && profile.cardProfileName == "input:analog-stereo";
  });
  if (first == snapshot.profiles.end() || second == snapshot.profiles.end() ||
      inputChanging == snapshot.profiles.end() || inputOnly == snapshot.profiles.end() ||
      first->observedOutputs.empty()) return;
  if (first->card.location != "pipetune.visibility.fixture" ||
      first->outputKey != "analog-stereo" || second->outputKey != "iec958-stereo" ||
      !first->preservesInput || !second->preservesInput || !inputOnly->preservesInput ||
      !inputOnly->outputKey.empty() || inputChanging->preservesInput ||
      !first->available || !second->available || first->observedOutputs.size() != 1 ||
      first->observedOutputs[0].channelPositions != std::vector<std::string>{"FL", "FR"}) {
    state.error = "card profiles or observed output layouts were incorrect";
    pw_main_loop_quit(state.loop);
    return;
  }
  if (state.step == 0 && first->active && !second->active) {
    state.card = first->card;
    state.originalInputRoutes = pipetune::pipeWireInputRoutes(*state.observer, state.card);
    if (state.originalInputRoutes.size() != 1) {
      state.error = "the synthetic input route was not captured";
      pw_main_loop_quit(state.loop);
      return;
    }
    const auto nodeCount = pipetune::pipeWireOutputProfileNodeCount(*state.observer,
        {.card = first->card, .cardProfileName = first->cardProfileName,
         .outputKey = first->outputKey, .allowMissingForRestore = false});
    if (nodeCount != 1) {
      state.error = "the selected profile did not report one output Node";
      pw_main_loop_quit(state.loop);
      return;
    }
    const auto inputOnlyNodeCount = pipetune::pipeWireOutputProfileNodeCount(*state.observer,
        {.card = first->card, .cardProfileName = inputOnly->cardProfileName,
         .outputKey = {}, .allowMissingForRestore = true});
    if (inputOnlyNodeCount != 0) {
      state.error = "the input-only profile did not report zero output Nodes";
      pw_main_loop_quit(state.loop);
      return;
    }
    const auto refused = pipetune::setPipeWireOutputProfile(*state.observer,
        {.card = first->card, .cardProfileName = inputChanging->cardProfileName,
         .outputKey = inputChanging->outputKey, .allowMissingForRestore = false});
    if (refused.empty()) {
      state.error = "an input-changing profile was accepted";
      pw_main_loop_quit(state.loop);
      return;
    }
    state.step = 1;
    state.error = pipetune::setPipeWireOutputProfile(*state.observer,
        {.card = first->card, .cardProfileName = "output:iec958-stereo+input:analog-stereo",
         .outputKey = "iec958-stereo", .allowMissingForRestore = false});
  } else if (state.step == 1 && !first->active && second->active && !second->observedOutputs.empty()) {
    if (pipetune::pipeWireInputRoutes(*state.observer, state.card) == state.originalInputRoutes) return;
    state.step = 2;
    state.error = pipetune::restorePipeWireInputRoutes(*state.observer, state.card, state.originalInputRoutes);
  } else if (state.step == 2 && !first->active && second->active &&
             pipetune::pipeWireInputRoutes(*state.observer, state.card) == state.originalInputRoutes) {
    state.step = 3;
    state.error = pipetune::setPipeWireOutputProfile(*state.observer,
        {.card = first->card, .cardProfileName = "output:analog-stereo+input:analog-stereo",
         .outputKey = "analog-stereo", .allowMissingForRestore = false});
  } else if (state.step == 3 && first->active && !second->active && !second->observedOutputs.empty()) {
    state.step = 4;
    state.error = pipetune::setPipeWireOutputProfile(*state.observer,
        {.card = first->card, .cardProfileName = inputOnly->cardProfileName,
         .outputKey = {}, .allowMissingForRestore = true});
  } else if (state.step == 4 && inputOnly->active && !first->active) {
    state.step = 5;
    state.error = pipetune::setPipeWireOutputProfile(*state.observer,
        {.card = first->card, .cardProfileName = first->cardProfileName,
         .outputKey = first->outputKey, .allowMissingForRestore = false});
  } else if (state.step == 5 && first->active && !inputOnly->active) {
    state.complete = true;
    pw_main_loop_quit(state.loop);
  }
  if (!state.error.empty()) pw_main_loop_quit(state.loop);
}

static void timedOut(void *data, std::uint64_t) {
  auto &state = *static_cast<State *>(data);
  state.error = "timed out waiting for the synthetic card profiles";
  pw_main_loop_quit(state.loop);
}

int main() {
  pw_init(nullptr, nullptr);
  auto state = State{};
  state.loop = pw_main_loop_new(nullptr);
  if (state.loop == nullptr) return 1;
  auto *context = pw_context_new(pw_main_loop_get_loop(state.loop), nullptr, 0);
  if (context == nullptr) return 1;
  auto *core = pw_context_connect(context, nullptr, 0);
  if (core == nullptr) return 1;
  auto observer = pipetune::observePipeWireOutputs(core, changed, &state);
  state.observer = observer.get();
  auto *timer = pw_loop_add_timer(pw_main_loop_get_loop(state.loop), timedOut, &state);
  auto deadline = timespec{.tv_sec = 10, .tv_nsec = 0};
  auto interval = timespec{};
  if (observer == nullptr || timer == nullptr ||
      pw_loop_update_timer(pw_main_loop_get_loop(state.loop), timer, &deadline, &interval, false) < 0) return 1;
  pw_main_loop_run(state.loop);
  observer.reset();
  pw_loop_destroy_source(pw_main_loop_get_loop(state.loop), timer);
  pw_core_disconnect(core);
  pw_context_destroy(context);
  pw_main_loop_destroy(state.loop);
  pw_deinit();
  if (!state.complete) std::cerr << state.error << '\n';
  return state.complete ? 0 : 1;
}
