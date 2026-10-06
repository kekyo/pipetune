/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pulse/pulseaudio.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

struct Probe;

struct Client {
  Probe *probe = nullptr;
  pa_context *context = nullptr;
  std::vector<std::string> sinks;
  std::string defaultSink;
  bool playback = false;
  bool querying = false;
  bool dirty = false;
  bool hidden = false;
  bool restored = false;
};

struct Probe {
  pa_mainloop *loop = nullptr;
  pa_stream *stream = nullptr;
  std::array<Client, 2> clients = {};
  std::uint64_t producedFrames = 0;
  bool playing = false;
  bool hidden = false;
  bool restored = false;
  bool stopping = false;
  std::string error;
};

static void query(Client &client);
static void startPlayback(Client &client);

static void fail(Probe &probe, const std::string &message) {
  if (probe.error.empty()) probe.error = message;
  pa_mainloop_quit(probe.loop, 1);
}

static void checkOperation(Client &client, pa_operation *operation) {
  if (operation == nullptr)
    fail(*client.probe, pa_strerror(pa_context_errno(client.context)));
  else pa_operation_unref(operation);
}

static void inspect(Client &client) {
  auto &probe = *client.probe;
  const auto combined = std::find(client.sinks.begin(), client.sinks.end(),
      "pipetune_probe_combined") != client.sinks.end();
  const auto physical = std::count_if(client.sinks.begin(), client.sinks.end(),
      [](const auto &name) { return name.starts_with("pipetune_probe_device_"); });
  if (!probe.hidden && combined && physical == 0 &&
      client.defaultSink == "pipetune_probe_combined") {
    if (client.playback && probe.stream == nullptr) startPlayback(client);
    if (probe.playing) {
      if (client.sinks.size() != 1) {
        auto message = std::string{"unexpected selectable PulseAudio outputs:"};
        for (const auto &name : client.sinks) message += " " + name;
        fail(probe, message);
        return;
      }
      client.hidden = true;
    }
    if (std::all_of(probe.clients.begin(), probe.clients.end(),
        [](const auto &view) { return view.hidden; })) {
      probe.hidden = true;
      std::cerr << "pulse-probe:outputs-hidden\n" << std::flush;
    }
  }
  if (probe.hidden && !combined) {
    if (!probe.stopping) {
      probe.stopping = true;
      pa_stream_set_state_callback(probe.stream, nullptr, nullptr);
      pa_stream_disconnect(probe.stream);
    }
    if (physical == 3 && client.defaultSink.starts_with("pipetune_probe_device_"))
      client.restored = true;
    if (std::all_of(probe.clients.begin(), probe.clients.end(),
        [](const auto &view) { return view.restored; })) {
      probe.restored = true;
      std::cerr << "pulse-probe:outputs-restored\n" << std::flush;
      pa_mainloop_quit(probe.loop, 0);
    }
  }
}

static void sinksReceived(pa_context *, const pa_sink_info *info, int eol, void *data) {
  auto &client = *static_cast<Client *>(data);
  if (eol < 0) {
    fail(*client.probe, "cannot enumerate PulseAudio sinks");
    return;
  }
  if (eol == 0) {
    if (info != nullptr && info->name != nullptr) client.sinks.emplace_back(info->name);
    return;
  }
  client.querying = false;
  // Reconcile a fresh snapshot when notifications arrived during enumeration.
  // Never treat a partial response as the current output selection.
  if (client.dirty) query(client);
  else inspect(client);
}

static void query(Client &client) {
  if (client.querying) {
    client.dirty = true;
    return;
  }
  client.querying = true;
  client.dirty = false;
  client.sinks.clear();
  checkOperation(client, pa_context_get_server_info(client.context,
      [](pa_context *, const pa_server_info *info, void *data) {
        auto &view = *static_cast<Client *>(data);
        if (info == nullptr) {
          fail(*view.probe, "cannot read the PulseAudio default output");
          return;
        }
        view.defaultSink = info->default_sink_name == nullptr ? "" : info->default_sink_name;
        checkOperation(view, pa_context_get_sink_info_list(view.context, sinksReceived, &view));
      }, &client));
}

static void produce(pa_stream *stream, std::size_t bytes, void *data) {
  auto &probe = *static_cast<Probe *>(data);
  void *buffer = nullptr;
  if (pa_stream_begin_write(stream, &buffer, &bytes) < 0) {
    fail(probe, "cannot obtain a PulseAudio playback buffer");
    return;
  }
  const auto frames = bytes / (2 * sizeof(float));
  auto *samples = static_cast<float *>(buffer);
  // The matching PipeWire receiver verifies channel identity and continuity
  // after the mock DSP doubles this stereo signal and creates channels 3/4.
  for (auto frame = std::size_t{0}; frame < frames; ++frame)
    for (auto channel = 0u; channel < 2; ++channel)
      samples[frame * 2 + channel] = static_cast<float>((channel + 1u) * 524288u +
          (probe.producedFrames + frame) % 524287u) / 268435456.0F * 0.5F;
  probe.producedFrames += frames;
  if (pa_stream_write(stream, buffer, frames * 2 * sizeof(float), nullptr, 0, PA_SEEK_RELATIVE) < 0)
    fail(probe, "cannot write PulseAudio playback samples");
}

static void startPlayback(Client &client) {
  auto &probe = *client.probe;
  const auto format = pa_sample_spec{PA_SAMPLE_FLOAT32NE, 48000, 2};
  auto *properties = pa_proplist_new();
  pa_proplist_sets(properties, PA_PROP_MEDIA_ROLE, "music");
  pa_proplist_sets(properties, "node.name", "pipetune_probe_signal");
  probe.stream = pa_stream_new_with_proplist(client.context, "PipeTune PulseAudio signal",
      &format, nullptr, properties);
  pa_proplist_free(properties);
  if (probe.stream == nullptr) {
    fail(probe, "cannot create PulseAudio playback");
    return;
  }
  pa_stream_set_write_callback(probe.stream, produce, &probe);
  pa_stream_set_state_callback(probe.stream, [](pa_stream *stream, void *data) {
    auto &state = *static_cast<Probe *>(data);
    const auto status = pa_stream_get_state(stream);
    if (status == PA_STREAM_FAILED) fail(state, "PulseAudio playback failed");
    if (status == PA_STREAM_READY) {
      state.playing = true;
      for (auto &view : state.clients)
        if (pa_context_get_state(view.context) == PA_CONTEXT_READY) query(view);
    }
  }, &probe);
  const auto attributes = pa_buffer_attr{UINT32_MAX, 8192, UINT32_MAX, UINT32_MAX, UINT32_MAX};
  // A normal desktop application leaves destination and volume to the server.
  if (pa_stream_connect_playback(probe.stream, nullptr, &attributes, PA_STREAM_NOFLAGS,
      nullptr, nullptr) < 0) fail(probe, "cannot connect default PulseAudio playback");
}

int main() {
  auto probe = Probe{};
  probe.loop = pa_mainloop_new();
  if (probe.loop == nullptr) return 1;
  for (auto index = 0u; index < probe.clients.size(); ++index) {
    auto &client = probe.clients[index];
    client.probe = &probe;
    client.playback = index == 1;
    client.context = pa_context_new(pa_mainloop_get_api(probe.loop),
        client.playback ? "PipeTune PulseAudio player probe" : "PipeTune PulseAudio settings probe");
    if (client.context == nullptr) {
      fail(probe, "cannot create PulseAudio context");
      break;
    }
    pa_context_set_state_callback(client.context, [](pa_context *context, void *data) {
      auto &view = *static_cast<Client *>(data);
      const auto state = pa_context_get_state(context);
      if (state == PA_CONTEXT_FAILED) fail(*view.probe, pa_strerror(pa_context_errno(context)));
      if (state != PA_CONTEXT_READY) return;
      pa_context_set_subscribe_callback(context,
          [](pa_context *, pa_subscription_event_type_t, std::uint32_t, void *value) {
            query(*static_cast<Client *>(value));
          }, &view);
      checkOperation(view, pa_context_subscribe(context,
          static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
          [](pa_context *, int success, void *value) {
            auto &subscribed = *static_cast<Client *>(value);
            if (!success) fail(*subscribed.probe, "cannot subscribe to PulseAudio outputs");
            else query(subscribed);
          }, &view));
    }, &client);
    if (pa_context_connect(client.context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) {
      fail(probe, "cannot connect to isolated PulseAudio server");
      break;
    }
  }
  if (probe.error.empty()) pa_mainloop_run(probe.loop, nullptr);
  if (probe.stream != nullptr) {
    pa_stream_set_state_callback(probe.stream, nullptr, nullptr);
    if (!probe.stopping) pa_stream_disconnect(probe.stream);
    pa_stream_unref(probe.stream);
  }
  for (auto &client : probe.clients) {
    if (client.context == nullptr) continue;
    pa_context_set_state_callback(client.context, nullptr, nullptr);
    pa_context_disconnect(client.context);
    pa_context_unref(client.context);
  }
  pa_mainloop_free(probe.loop);
  if (!probe.error.empty()) std::cerr << probe.error << '\n';
  std::cout << "{\"clients\":2,\"producedFrames\":" << probe.producedFrames
            << ",\"outputsHidden\":" << (probe.hidden ? "true" : "false")
            << ",\"outputsRestored\":" << (probe.restored ? "true" : "false") << "}\n";
  return probe.hidden && probe.restored && probe.error.empty() ? 0 : 1;
}
