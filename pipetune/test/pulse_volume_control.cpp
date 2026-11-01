/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pulse/pulseaudio.h>

#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

struct Control {
  pa_mainloop *loop = nullptr;
  pa_context *context = nullptr;
  std::string command;
  std::string target;
  unsigned value = 0;
  bool first = true;
  bool querying = false;
  bool dirty = false;
  bool unexpected = false;
  unsigned sinks = 0;
  std::ostringstream snapshot;
};

static void query(Control &control);

static void fail(Control &control) {
  std::cerr << pa_strerror(pa_context_errno(control.context)) << '\n';
  pa_mainloop_quit(control.loop, 1);
}

static void operation(Control &control, pa_operation *pending) {
  if (pending == nullptr) fail(control);
  else pa_operation_unref(pending);
}

static void completed(pa_context *, int success, void *data) {
  auto &control = *static_cast<Control *>(data);
  if (!success) fail(control);
  else pa_mainloop_quit(control.loop, 0);
}

static void sinkReceived(pa_context *, const pa_sink_info *info, int end, void *data) {
  auto &control = *static_cast<Control *>(data);
  if (end < 0) return fail(control);
  if (end > 0) {
    if (control.command == "list") {
      control.querying = false;
      if (control.dirty) query(control);
      else if (control.target.empty() || (!control.unexpected && control.sinks == 2)) {
        std::cout << control.snapshot.str() << "]}\n";
        pa_mainloop_quit(control.loop, 0);
      }
    }
    return;
  }
  if (info == nullptr) return fail(control);
  if (control.command == "volume") {
    auto volume = pa_cvolume{};
    pa_cvolume_set(&volume, info->volume.channels, control.value * PA_VOLUME_NORM / 100);
    operation(control, pa_context_set_sink_volume_by_name(control.context,
        control.target.c_str(), &volume, completed, &control));
    return;
  }
  ++control.sinks;
  if (!control.target.empty() && info->name != control.target && info->name != control.target + ".direct")
    control.unexpected = true;
  if (!control.first) control.snapshot << ',';
  control.first = false;
  control.snapshot << "{\"name\":" << std::quoted(info->name) << ",\"mute\":"
      << (info->mute ? "true" : "false") << ",\"volumes\":[";
  for (auto channel = 0U; channel < info->volume.channels; ++channel) {
    if (channel != 0) control.snapshot << ',';
    control.snapshot << info->volume.values[channel];
  }
  control.snapshot << "]}";
}

static void query(Control &control) {
  if (control.querying) { control.dirty = true; return; }
  control.querying = true;
  control.dirty = false;
  control.unexpected = false;
  control.sinks = 0;
  control.first = true;
  control.snapshot.str("");
  operation(control, pa_context_get_server_info(control.context,
        [](pa_context *, const pa_server_info *info, void *data) {
          auto &control = *static_cast<Control *>(data);
          if (info == nullptr) return fail(control);
          control.snapshot << "{\"default\":" << std::quoted(info->default_sink_name == nullptr ? "" : info->default_sink_name)
              << ",\"sinks\":[";
          operation(control, pa_context_get_sink_info_list(control.context, sinkReceived, &control));
        }, &control));
}

static void connected(pa_context *context, void *data) {
  auto &control = *static_cast<Control *>(data);
  const auto state = pa_context_get_state(context);
  if (state == PA_CONTEXT_FAILED) return fail(control);
  if (state != PA_CONTEXT_READY) return;
  if (control.command == "list") {
    pa_context_set_subscribe_callback(context, [](pa_context *, pa_subscription_event_type_t, uint32_t, void *data) {
      query(*static_cast<Control *>(data));
    }, &control);
    operation(control, pa_context_subscribe(context,
        static_cast<pa_subscription_mask_t>(PA_SUBSCRIPTION_MASK_SINK | PA_SUBSCRIPTION_MASK_SERVER),
        [](pa_context *, int success, void *data) {
          auto &control = *static_cast<Control *>(data);
          if (success) query(control); else fail(control);
        }, &control));
  } else if (control.command == "volume") {
    operation(control, pa_context_get_sink_info_by_name(context, control.target.c_str(), sinkReceived, &control));
  } else if (control.command == "mute") {
    operation(control, pa_context_set_sink_mute_by_name(context, control.target.c_str(), control.value != 0, completed, &control));
  } else {
    operation(control, pa_context_set_default_sink(context, control.target.c_str(), completed, &control));
  }
}

int main(int argc, char **argv) {
  if (argc < 2) return 2;
  auto control = Control{};
  control.command = argv[1];
  if (control.command != "list" && control.command != "volume" && control.command != "mute" && control.command != "default") return 2;
  if (control.command != "list" && argc < 3) return 2;
  if ((control.command == "volume" || control.command == "mute") && argc < 4) return 2;
  if (argc >= 3) control.target = argv[2];
  if (argc >= 4) control.value = static_cast<unsigned>(std::stoul(argv[3]));
  if (control.value > 100) return 2;
  control.loop = pa_mainloop_new();
  control.context = pa_context_new(pa_mainloop_get_api(control.loop), "PipeTune desktop volume test");
  pa_context_set_state_callback(control.context, connected, &control);
  auto result = 1;
  if (pa_context_connect(control.context, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) >= 0)
    pa_mainloop_run(control.loop, &result);
  else fail(control);
  pa_context_disconnect(control.context);
  pa_context_unref(control.context);
  pa_mainloop_free(control.loop);
  return result;
}
