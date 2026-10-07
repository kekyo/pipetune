/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
/* Isolated duplex fixture using synthetic audio only; no hardware is opened. */
#define _GNU_SOURCE
#include <sys/types.h>
#include <signal.h>
#include <stdio.h>
#include <pipewire/pipewire.h>
#include <spa/monitor/utils.h>
#include <spa/param/profile.h>
#include <spa/param/route.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/param/props.h>
#include <spa/param/audio/raw.h>

struct fixture {
    struct spa_device device;
    struct spa_hook_list listeners;
    struct pw_main_loop *loop;
    struct spa_param_info params[4];
    struct pw_core *core;
    struct pw_node *nodes[2];
    struct spa_hook bound_listener;
    int active_routes[2];
    float volumes[2][2];
    bool mute[2];
    bool save[2];
};

static const struct spa_dict_item properties[] = {
    { "device.name", "pipetune.visibility.fixture" },
    { "device.description", "Visibility duplex fixture" },
    { "media.class", "Audio/Device" },
    { "device.api", "pipetune-fixture" },
};
static const struct spa_dict dictionary = SPA_DICT_INIT_ARRAY(properties);

static void emit_info(struct fixture *self) {
    struct spa_device_info info = {
        .version = SPA_VERSION_DEVICE_INFO,
        .change_mask = SPA_DEVICE_CHANGE_MASK_PROPS | SPA_DEVICE_CHANGE_MASK_PARAMS,
        .props = &dictionary,
        .params = self->params,
        .n_params = 4,
    };
    spa_device_emit_info(&self->listeners, &info);
}

static int add_listener(void *object, struct spa_hook *listener,
        const struct spa_device_events *events, void *data) {
    struct fixture *self = object;
    struct spa_hook_list saved;
    spa_hook_list_isolate(&self->listeners, &saved, listener, events, data);
    emit_info(self);
    spa_hook_list_join(&self->listeners, &saved);
    return 0;
}

static int sync_device(void *object, int seq) {
    struct fixture *self = object;
    spa_device_emit_result(&self->listeners, seq, 0, 0, NULL);
    return 0;
}

static struct spa_pod *profile(struct spa_pod_builder *builder, uint32_t id) {
    struct spa_pod_frame frames[2];
    const int output = 0, input = 1;
    spa_pod_builder_push_object(builder, &frames[0], SPA_TYPE_OBJECT_ParamProfile, id);
    spa_pod_builder_add(builder,
        SPA_PARAM_PROFILE_index, SPA_POD_Int(0),
        SPA_PARAM_PROFILE_name, SPA_POD_String("duplex"),
        SPA_PARAM_PROFILE_description, SPA_POD_String("Stereo duplex"),
        SPA_PARAM_PROFILE_priority, SPA_POD_Int(100),
        SPA_PARAM_PROFILE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes), 0);
    spa_pod_builder_prop(builder, SPA_PARAM_PROFILE_classes, 0);
    spa_pod_builder_push_struct(builder, &frames[1]);
    spa_pod_builder_int(builder, 2);
    spa_pod_builder_add_struct(builder, SPA_POD_String("Audio/Sink"), SPA_POD_Int(1),
        SPA_POD_String("card.profile.devices"), SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &output));
    spa_pod_builder_add_struct(builder, SPA_POD_String("Audio/Source"), SPA_POD_Int(1),
        SPA_POD_String("card.profile.devices"), SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &input));
    spa_pod_builder_pop(builder, &frames[1]);
    return spa_pod_builder_pop(builder, &frames[0]);
}

static struct spa_pod *route(struct fixture *self, struct spa_pod_builder *builder, uint32_t id, uint32_t index) {
    const char *names[] = { "fixture-output-analog", "fixture-output-hdmi", "fixture-input-mic", "fixture-input-line" };
    const char *descriptions[] = { "Fixture Analog Output", "Fixture HDMI Output", "Fixture Microphone", "Fixture Line Input" };
    const int profile_index = 0, device = index < 2 ? 0 : 1;
    struct spa_pod_frame frame;
    spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_ParamRoute, id);
    spa_pod_builder_add(builder,
        SPA_PARAM_ROUTE_index, SPA_POD_Int(index),
        SPA_PARAM_ROUTE_direction, SPA_POD_Id(index < 2 ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT),
        SPA_PARAM_ROUTE_name, SPA_POD_String(names[index]),
        SPA_PARAM_ROUTE_description, SPA_POD_String(descriptions[index]),
        SPA_PARAM_ROUTE_priority, SPA_POD_Int(100),
        SPA_PARAM_ROUTE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes),
        SPA_PARAM_ROUTE_profiles, SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &profile_index),
        SPA_PARAM_ROUTE_devices, SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &device), 0);
    if (id == SPA_PARAM_Route) {
        spa_pod_builder_add(builder,
            SPA_PARAM_ROUTE_device, SPA_POD_Int(device),
            SPA_PARAM_ROUTE_profile, SPA_POD_Int(profile_index),
            SPA_PARAM_ROUTE_save, SPA_POD_Bool(self->save[device]), 0);
        const uint32_t map[] = {SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR};
        spa_pod_builder_prop(builder, SPA_PARAM_ROUTE_props, 0);
        spa_pod_builder_add_object(builder, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_mute, SPA_POD_Bool(self->mute[device]),
            SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, self->volumes[device]),
            SPA_PROP_channelMap, SPA_POD_Array(sizeof(uint32_t), SPA_TYPE_Id, 2, map),
            SPA_PROP_volumeBase, SPA_POD_Float(1.0f), SPA_PROP_volumeStep, SPA_POD_Float(0.01f));
    }
    return spa_pod_builder_pop(builder, &frame);
}

static int enum_params(void *object, int seq, uint32_t id, uint32_t start,
        uint32_t maximum, const struct spa_pod *filter) {
    struct fixture *self = object;
    const uint32_t length = id == SPA_PARAM_EnumRoute ? 4 : id == SPA_PARAM_Route ? 2 : 1;
    uint32_t emitted = 0;
    for (uint32_t index = start; index < length && emitted < maximum; ++index) {
        uint8_t buffer[4096];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_pod *pod;
        if (id == SPA_PARAM_EnumProfile || id == SPA_PARAM_Profile)
            pod = profile(&builder, id);
        else if (id == SPA_PARAM_EnumRoute || id == SPA_PARAM_Route)
            pod = route(self, &builder, id, id == SPA_PARAM_Route ? (uint32_t)self->active_routes[index] : index);
        else return -ENOENT;
        struct spa_result_device_params result = { .id = id, .index = index, .next = index + 1 };
        if (spa_pod_filter(&builder, &result.param, pod, filter) < 0) continue;
        spa_device_emit_result(&self->listeners, seq, 0, SPA_RESULT_TYPE_DEVICE_PARAMS, &result);
        ++emitted;
    }
    return 0;
}

static int set_param(void *object, uint32_t id, uint32_t flags, const struct spa_pod *param) {
    (void)flags;
    struct fixture *self = object;
    if (id == SPA_PARAM_Profile) return 0;
    if (id != SPA_PARAM_Route) return -ENOENT;
    int index = -1, device = -1;
    bool save = false;
    struct spa_pod *props = NULL;
    if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
            SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
            SPA_PARAM_ROUTE_device, SPA_POD_Int(&device),
            SPA_PARAM_ROUTE_save, SPA_POD_OPT_Bool(&save),
            SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0 ||
            device < 0 || device > 1 || index < device * 2 || index >= device * 2 + 2) return -EINVAL;
    bool changed = self->active_routes[device] != index || self->save[device] != save;
    self->active_routes[device] = index;
    self->save[device] = save;
    if (props) {
        bool mute = self->mute[device];
        struct spa_pod *volumes = NULL;
        spa_pod_parse_object(props, SPA_TYPE_OBJECT_Props, NULL,
            SPA_PROP_mute, SPA_POD_OPT_Bool(&mute), SPA_PROP_channelVolumes, SPA_POD_OPT_Pod(&volumes));
        changed |= mute != self->mute[device];
        self->mute[device] = mute;
        if (volumes && spa_pod_is_array(volumes) && SPA_POD_ARRAY_VALUE_TYPE(volumes) == SPA_TYPE_Float &&
            (SPA_POD_ARRAY_N_VALUES(volumes) == 1 || SPA_POD_ARRAY_N_VALUES(volumes) == 2)) {
            const float *values = SPA_POD_ARRAY_VALUES(volumes);
            for (unsigned i = 0; i < 2; ++i) {
                const float value = values[SPA_POD_ARRAY_N_VALUES(volumes) == 1 ? 0 : i];
                changed |= value != self->volumes[device][i];
                self->volumes[device][i] = value;
            }
        }
    }
    if (self->nodes[device]) {
        uint8_t buffer[512];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *p = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
            SPA_PROP_mute, SPA_POD_Bool(self->mute[device]),
            SPA_PROP_channelVolumes, SPA_POD_Array(sizeof(float), SPA_TYPE_Float, 2, self->volumes[device]));
        pw_node_set_param(self->nodes[device], SPA_PARAM_Props, 0, p);
    }
    printf("route-set device=%d index=%d mute=%d volume=%f,%f changed=%d\n", device, index, self->mute[device], self->volumes[device][0], self->volumes[device][1], changed);
    fflush(stdout);
    if (changed) { self->params[3].flags ^= SPA_PARAM_INFO_SERIAL; emit_info(self); }
    return 0;
}

static const struct spa_device_methods methods = {
    .version = SPA_VERSION_DEVICE_METHODS, .add_listener = add_listener,
    .sync = sync_device, .enum_params = enum_params, .set_param = set_param,
};

static void on_signal(void *data, int number) {
    (void)number;
    struct fixture *self = data;
    pw_main_loop_quit(self->loop);
}

static void card_bound(void *data, uint32_t id) {
    struct fixture *self = data;
    char card[32];
    snprintf(card, sizeof(card), "%u", id);
    for (unsigned direction = 0; direction < 2; ++direction) {
        struct pw_properties *props = pw_properties_new(
            "factory.name", "support.null-audio-sink",
            "node.name", direction ? "fixture.input" : "fixture.output",
            "node.description", direction ? "Fixture Input" : "Fixture Output",
            "media.class", direction ? "Audio/Source/Virtual" : "Audio/Sink",
            "device.id", card, "card.profile.device", direction ? "1" : "0",
            "audio.position", "[ FL FR ]", "audio.rate", "48000",
            "monitor.channel-volumes", "true",
            "node.virtual", "false", "priority.session", "1000", NULL);
        self->nodes[direction] = pw_core_create_object(self->core, "adapter", PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &props->dict, 0);
        pw_properties_free(props);
    }
    printf("card-bound=%u\n", id); fflush(stdout);
}
static const struct pw_proxy_events bound_events = { .version = PW_VERSION_PROXY_EVENTS, .bound = card_bound };

int main(int argc, char **argv) {
    pw_init(&argc, &argv);
    struct fixture self = { .params = {
        { .id = SPA_PARAM_EnumProfile, .flags = SPA_PARAM_INFO_READ },
        { .id = SPA_PARAM_Profile, .flags = SPA_PARAM_INFO_READWRITE },
        { .id = SPA_PARAM_EnumRoute, .flags = SPA_PARAM_INFO_READ },
        { .id = SPA_PARAM_Route, .flags = SPA_PARAM_INFO_READWRITE },
    }, .active_routes = {0, 2}, .volumes = {{1.0f, 1.0f}, {1.0f, 1.0f}} };
    self.device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device, SPA_VERSION_DEVICE, &methods, &self);
    spa_hook_list_init(&self.listeners);
    self.loop = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(self.loop);
    struct spa_source *signals[] = {
        pw_loop_add_signal(loop, SIGTERM, on_signal, &self),
        pw_loop_add_signal(loop, SIGINT, on_signal, &self),
    };
    struct pw_context *context = pw_context_new(loop, NULL, 0);
    /* Real hardware belongs to WirePlumber, which can see the internal capture
     * endpoint. Give this synthetic device owner the same visibility while
     * leaving the recording and GNOME clients under the normal policy. */
    struct pw_core *core = pw_context_connect(context,
        pw_properties_new("application.process.binary", "pipetune",
            "application.name", "PipeTune duplex fixture", NULL), 0);
    if (!core) return 1;
    self.core = core;
    struct pw_proxy *proxy = pw_core_export(core, SPA_TYPE_INTERFACE_Device, &dictionary, &self.device, 0);
    if (!proxy) return 2;
    pw_proxy_add_listener(proxy, &self.bound_listener, &bound_events, &self);
    pw_main_loop_run(self.loop);
    for (unsigned i = 0; i < 2; ++i) if (self.nodes[i]) pw_proxy_destroy((struct pw_proxy *)self.nodes[i]);
    spa_hook_remove(&self.bound_listener);
    pw_proxy_destroy(proxy);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    for (unsigned i = 0; i < 2; ++i) pw_loop_destroy_source(loop, signals[i]);
    pw_main_loop_destroy(self.loop);
    pw_deinit();
    return 0;
}
