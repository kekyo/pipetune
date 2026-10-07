/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include "pipewire_output_inventory.h"

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/props.h>

#include <algorithm>
#include <array>
#include <iostream>

struct TestState {
  pw_main_loop *loop = nullptr;
  pw_core *core = nullptr;
  pw_stream *stream = nullptr;
  pipetune::OutputConfiguration saved;
  unsigned step = 0;
  std::string error;
};

static void fail(TestState &state, std::string error) {
  state.error = std::move(error);
  pw_main_loop_quit(state.loop);
}

static bool configureOutput(TestState &state, unsigned channels, bool connect) {
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  auto format = spa_audio_info_raw{};
  format.format = SPA_AUDIO_FORMAT_F32P;
  format.rate = 48000;
  format.channels = channels;
  for (auto index = 0U; index < channels; ++index) format.position[index] = SPA_AUDIO_CHANNEL_AUX0 + index;
  const auto *parameter = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
  if (!connect) return pw_stream_update_params(state.stream, &parameter, 1) >= 0;
  return pw_stream_connect(state.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
      static_cast<pw_stream_flags>(PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_NO_CONVERT), &parameter, 1) >= 0;
}

static bool createOutput(TestState &state, unsigned channels) {
  state.stream = pw_stream_new(state.core, "Inventory test output", pw_properties_new(
      PW_KEY_NODE_NAME, "inventory.dynamic", PW_KEY_NODE_DESCRIPTION, "Dynamic output",
      PW_KEY_MEDIA_CLASS, "Audio/Sink", "node.device.profile.name", "custom", nullptr));
  return state.stream != nullptr && configureOutput(state, channels, true);
}

// This fixture reports device controls directly. An inactive converter may
// defer volume notifications until its audio formats have been negotiated.
static bool publishVolume(TestState &state, bool muted) {
  auto storage = std::array<std::uint8_t, 1024>{};
  auto builder = SPA_POD_BUILDER_INIT(storage.data(), storage.size());
  const auto gains = std::array<float, 6>{0, 0.125F, 0.25F, 0.5F, 1, 2};
  const auto *parameter = static_cast<const spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
      SPA_PROP_mute, SPA_POD_Bool(muted), SPA_PROP_volume, SPA_POD_Float(1.0F),
      SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, gains.size(), gains.data())));
  return pw_stream_update_params(state.stream, &parameter, 1) >= 0;
}

static void changed(const pipetune::OutputInventoryResult &snapshot, void *data) {
  auto &state = *static_cast<TestState *>(data);
  if (!snapshot.error.empty()) return fail(state, snapshot.error);
  const auto output = std::find_if(snapshot.outputs.begin(), snapshot.outputs.end(),
      [](const auto &entry) { return entry.nodeName == "inventory.dynamic"; });
  const auto present = output != snapshot.outputs.end();
  const auto volume = present ? std::find_if(snapshot.volumes.begin(), snapshot.volumes.end(),
      [&output](const auto &entry) { return entry.nodeSerial == output->nodeSerial; }) : snapshot.volumes.end();
  if (state.step == 0) {
    state.step = 1;
    if (!createOutput(state, 6)) fail(state, "could not create the dynamic output");
  } else if (state.step == 1 && present) {
    if (output->device.channelPositions != std::vector<std::string>{"AUX0", "AUX1", "AUX2", "AUX3", "AUX4", "AUX5"})
      return fail(state, "the first synchronized snapshot must include the complete parameter-derived layout");
    const auto selected = pipetune::appendConfiguredOutput({},
        {.id = "saved", .enabled = true, .device = output->device});
    if (!selected.error.empty()) return fail(state, selected.error);
    state.saved = selected.configuration;
    state.step = 2;
    const auto item = spa_dict_item{PW_KEY_NODE_DESCRIPTION, "Renamed output"};
    const auto dictionary = SPA_DICT_INIT(&item, 1);
    if (pw_stream_update_properties(state.stream, &dictionary) < 0) fail(state, "could not rename the output");
  } else if (state.step == 2 && present && output->device.name == "Renamed output") {
    if (pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
        pipetune::OutputConnectionState::connected) return fail(state, "a display-name change broke the saved binding");
    state.step = 3;
    if (!configureOutput(state, 8, false)) fail(state, "could not update the live channel layout");
  } else if (state.step == 3 && present && output->device.channelPositions.size() == 8) {
    if (pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
        pipetune::OutputConnectionState::profileMismatch) return fail(state, "live layout change must invalidate saved routing");
    state.step = 4;
    if (!configureOutput(state, 6, false)) fail(state, "could not restore the live channel layout");
  } else if (state.step == 4 && present && output->device.channelPositions.size() == 6) {
    if (pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
        pipetune::OutputConnectionState::connected) return fail(state, "restoring the layout must recover saved routing");
    state.step = 5;
    pw_stream_destroy(state.stream);
    state.stream = nullptr;
  } else if ((state.step == 5 || state.step == 7) && !present) {
    if (pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
        pipetune::OutputConnectionState::missing) return fail(state, "disconnected output was not reported missing");
    const auto channels = state.step == 5 ? 8U : 6U;
    ++state.step;
    if (!createOutput(state, channels)) fail(state, "could not reconnect the dynamic output");
  } else if (state.step == 6 && present) {
    if (output->device.channelPositions.size() != 8 ||
        pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
            pipetune::OutputConnectionState::profileMismatch)
      return fail(state, "a changed layout must be detected without reassigning saved channels");
    state.step = 7;
    pw_stream_destroy(state.stream);
    state.stream = nullptr;
  } else if (state.step == 8 && present) {
    if (pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
        pipetune::OutputConnectionState::connected || state.saved.channels.size() != 6)
      return fail(state, "reconnection must restore the original binding and six fixed slots");
    state.step = 9;
    if (!publishVolume(state, true)) fail(state, "could not report test device gain and mute");
  } else if (state.step == 9 && volume != snapshot.volumes.end() && volume->muted == true) {
    if (volume->channelVolumes != std::vector<float>{0, 0.125F, 0.25F, 0.5F, 1, 2})
      return fail(state, "device gain reports must retain every channel, including silence and amplification");
    state.step = 10;
    if (!publishVolume(state, false)) fail(state, "could not unmute the test device");
  } else if (state.step == 10 && volume != snapshot.volumes.end() && volume->muted == false) {
    if (volume->channelVolumes != std::vector<float>{0, 0.125F, 0.25F, 0.5F, 1, 2} ||
        pipetune::resolveConfiguredOutputs(state.saved, snapshot.outputs)[0].state !=
            pipetune::OutputConnectionState::connected)
      return fail(state, "a mute-only change must retain device gains and saved routing");
    state.step = 11;
    pw_stream_destroy(state.stream);
    state.stream = nullptr;
  } else if (state.step == 11 && !present) {
    for (const auto &entry : snapshot.volumes) {
      if (std::none_of(snapshot.outputs.begin(), snapshot.outputs.end(),
          [&entry](const auto &device) { return device.nodeSerial == entry.nodeSerial; }))
        return fail(state, "disconnected devices must not leave stale volume reports");
    }
    state.step = 12;
    pw_main_loop_quit(state.loop);
  }
}

static void timedOut(void *data, std::uint64_t) {
  auto &state = *static_cast<TestState *>(data);
  fail(state, "inventory observer timed out at step " + std::to_string(state.step));
}

int main() {
  pw_init(nullptr, nullptr);
  auto state = TestState{};
  state.loop = pw_main_loop_new(nullptr);
  if (state.loop == nullptr) return 1;
  auto *context = pw_context_new(pw_main_loop_get_loop(state.loop), nullptr, 0);
  if (context == nullptr) return 1;
  state.core = pw_context_connect(context, nullptr, 0);
  if (state.core == nullptr) return 1;
  auto observer = pipetune::observePipeWireOutputs(state.core, changed, &state);
  auto *timer = pw_loop_add_timer(pw_main_loop_get_loop(state.loop), timedOut, &state);
  auto deadline = timespec{.tv_sec = 10, .tv_nsec = 0};
  auto interval = timespec{};
  if (observer == nullptr || timer == nullptr ||
      pw_loop_update_timer(pw_main_loop_get_loop(state.loop), timer, &deadline, &interval, false) < 0) return 1;
  pw_main_loop_run(state.loop);
  observer.reset();
  if (state.stream != nullptr) pw_stream_destroy(state.stream);
  pw_loop_destroy_source(pw_main_loop_get_loop(state.loop), timer);
  pw_core_disconnect(state.core);
  pw_context_destroy(context);
  pw_main_loop_destroy(state.loop);
  pw_deinit();
  if (!state.error.empty()) std::cerr << state.error << '\n';
  return state.error.empty() && state.step == 12 ? 0 : 1;
}
