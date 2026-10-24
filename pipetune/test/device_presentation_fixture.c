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
#include <stdlib.h>
#include <string.h>
#include <pipewire/pipewire.h>
#include <spa/monitor/utils.h>
#include <spa/param/profile.h>
#include <spa/param/route.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>
#include <spa/param/props.h>
#include <spa/param/audio/raw.h>
#include <spa/param/audio/format-utils.h>

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
    int active_profile;
    bool save_profile;
    bool output_profile_variant;
    bool recreate_output_on_profile_change;
    bool omit_profile_classes;
    bool hold_output_node;
    uint32_t card_id;
    bool monitor_optical_pcm;
    struct pw_stream *optical_monitor;
    struct spa_hook optical_monitor_listener;
    uint32_t optical_pcm_frames;
    bool optical_sample_printed;
    bool optical_format_printed;
};

static void capture_optical_pcm(void *data) {
    struct fixture *self = data;
    struct pw_buffer *queued = pw_stream_dequeue_buffer(self->optical_monitor);
    if (!queued) return;
    if (!self->optical_format_printed && queued->buffer) {
        printf("optical-monitor-format=%u,%u,%u\n", queued->buffer->n_datas,
            queued->buffer->n_datas > 0 && queued->buffer->datas[0].chunk ? queued->buffer->datas[0].chunk->stride : 0,
            queued->buffer->n_datas > 1 && queued->buffer->datas[1].chunk ? queued->buffer->datas[1].chunk->stride : 0);
        fflush(stdout);
        self->optical_format_printed = true;
    }
    if (queued->buffer && queued->buffer->n_datas == 2) {
        const struct spa_data *left = &queued->buffer->datas[0];
        const struct spa_data *right = &queued->buffer->datas[1];
        if (left->data && right->data && left->chunk && right->chunk &&
                left->chunk->stride == sizeof(float) && right->chunk->stride == sizeof(float) &&
                left->chunk->offset <= left->maxsize && right->chunk->offset <= right->maxsize &&
                left->chunk->size <= left->maxsize - left->chunk->offset &&
                right->chunk->size <= right->maxsize - right->chunk->offset) {
            const uint32_t frames = SPA_MIN(left->chunk->size, right->chunk->size) / sizeof(float);
            for (uint32_t frame = 0; frame < frames; ++frame) {
                float a = 0, b = 0;
                memcpy(&a, (const char *)left->data + left->chunk->offset + frame * sizeof(float), sizeof(float));
                memcpy(&b, (const char *)right->data + right->chunk->offset + frame * sizeof(float), sizeof(float));
                if (!self->optical_sample_printed && (a > 0.001f || a < -0.001f || b > 0.001f || b < -0.001f)) {
                    printf("optical-pcm-sample=%f,%f\n", a, b); fflush(stdout);
                    self->optical_sample_printed = true;
                }
                const float ratio = a != 0.0f ? b / a + 2.0f : 10.0f;
                if ((a > 0.005f || a < -0.005f) && (b > 0.005f || b < -0.005f) &&
                        ratio > -0.1f && ratio < 0.1f)
                    ++self->optical_pcm_frames;
                else self->optical_pcm_frames = 0;
                if (self->optical_pcm_frames == 4096) {
                    printf("optical-pcm-frames=4096\n");
                    fflush(stdout);
                }
            }
        }
    }
    pw_stream_queue_buffer(self->optical_monitor, queued);
}

static void optical_monitor_state(void *data, enum pw_stream_state old_state,
        enum pw_stream_state state, const char *error) {
    (void)data; (void)old_state;
    printf("optical-monitor-state=%d error=%s\n", (int)state, error ? error : "");
    fflush(stdout);
}

static void create_optical_monitor(struct fixture *self) {
    if (!self->monitor_optical_pcm || self->optical_monitor) return;
    self->optical_monitor = pw_stream_new(self->core, "Fixture optical monitor", pw_properties_new(
        PW_KEY_NODE_NAME, "fixture.optical.monitor", PW_KEY_MEDIA_CLASS, "Stream/Input/Audio",
        PW_KEY_TARGET_OBJECT, "fixture.output.iec958", PW_KEY_STREAM_CAPTURE_SINK, "true",
        "node.link-group", "pipetune.profile-fixture-monitor",
        "adapter.auto-port-config", "{ mode = dsp position = preserve }",
        "node.dont-fallback", "true", "node.dont-move", "true", NULL));
    if (!self->optical_monitor) {
        printf("optical-monitor-create-failed\n"); fflush(stdout);
        return;
    }
    static const struct pw_stream_events events = {
        .version = PW_VERSION_STREAM_EVENTS, .state_changed = optical_monitor_state,
        .process = capture_optical_pcm,
    };
    pw_stream_add_listener(self->optical_monitor, &self->optical_monitor_listener, &events, self);
    struct spa_audio_info_raw format = {0};
    format.format = SPA_AUDIO_FORMAT_F32P;
    format.rate = 48000;
    format.channels = 2;
    format.position[0] = SPA_AUDIO_CHANNEL_FL;
    format.position[1] = SPA_AUDIO_CHANNEL_FR;
    uint8_t storage[512];
    struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(storage, sizeof(storage));
    const struct spa_pod *parameter = spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format);
    if (pw_stream_connect(self->optical_monitor, PW_DIRECTION_INPUT, PW_ID_ANY,
            PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, &parameter, 1) < 0) {
        printf("optical-monitor-connect-failed\n"); fflush(stdout);
        spa_hook_remove(&self->optical_monitor_listener);
        pw_stream_destroy(self->optical_monitor);
        self->optical_monitor = NULL;
    }
}

static void create_output_node(struct fixture *self) {
    if (self->output_profile_variant && self->active_profile == 3) return;
    if (self->hold_output_node) {
        printf("output-node-held=%d\n", self->active_profile);
        fflush(stdout);
        return;
    }
    char card[32];
    snprintf(card, sizeof(card), "%u", self->card_id);
    const char *name = self->active_profile == 1 ? "fixture.output.iec958" : "fixture.output";
    const char *profile = self->active_profile == 1 ? "iec958-stereo" : "analog-stereo";
    struct pw_properties *props = pw_properties_new(
        "factory.name", "support.null-audio-sink", "node.name", name,
        "node.description", "Fixture Output", "media.class", "Audio/Sink",
        "device.id", card, "card.profile.device", "0",
        "node.device.profile.name", profile,
        "audio.position", "[ FL FR ]", "audio.rate", "48000",
        "monitor.channel-volumes", "true", "node.virtual", "false",
        "priority.session", "1000", NULL);
    self->nodes[0] = pw_core_create_object(self->core, "adapter", PW_TYPE_INTERFACE_Node,
        PW_VERSION_NODE, &props->dict, 0);
    pw_properties_free(props);
}

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

static struct spa_pod *profile(struct fixture *self, struct spa_pod_builder *builder, uint32_t id, int index) {
    struct spa_pod_frame frames[2];
    const int output = 0, input = 1;
    spa_pod_builder_push_object(builder, &frames[0], SPA_TYPE_OBJECT_ParamProfile, id);
    const char *name = self->output_profile_variant ?
        (index == 0 ? "output:analog-stereo+input:analog-stereo" :
         index == 1 ? "output:iec958-stereo+input:analog-stereo" :
         index == 3 ? "input:analog-stereo" :
         "output:analog-stereo+input:other") :
        (index == 0 ? "duplex" : "duplex-alternative");
    spa_pod_builder_add(builder,
        SPA_PARAM_PROFILE_index, SPA_POD_Int(index),
        SPA_PARAM_PROFILE_name, SPA_POD_String(name),
        SPA_PARAM_PROFILE_description, SPA_POD_String(index == 0 ? "Stereo duplex" : "Alternative stereo duplex"),
        SPA_PARAM_PROFILE_priority, SPA_POD_Int(index == 0 ? 100 : 90),
        SPA_PARAM_PROFILE_save, SPA_POD_Bool(self->save_profile),
        SPA_PARAM_PROFILE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes), 0);
    if (!self->omit_profile_classes) {
        spa_pod_builder_prop(builder, SPA_PARAM_PROFILE_classes, 0);
        spa_pod_builder_push_struct(builder, &frames[1]);
        const bool input_only = self->output_profile_variant && index == 3;
        spa_pod_builder_int(builder, input_only ? 1 : 2);
        if (!input_only) spa_pod_builder_add_struct(builder, SPA_POD_String("Audio/Sink"), SPA_POD_Int(1),
            SPA_POD_String("card.profile.devices"), SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &output));
        spa_pod_builder_add_struct(builder, SPA_POD_String("Audio/Source"), SPA_POD_Int(1),
            SPA_POD_String("card.profile.devices"), SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &input));
        spa_pod_builder_pop(builder, &frames[1]);
    }
    return spa_pod_builder_pop(builder, &frames[0]);
}

static struct spa_pod *route(struct fixture *self, struct spa_pod_builder *builder, uint32_t id, uint32_t index) {
    const char *names[] = { "fixture-output-analog", "fixture-output-hdmi", "fixture-input-mic", "fixture-input-line" };
    const char *descriptions[] = { "Fixture Analog Output", "Fixture HDMI Output", "Fixture Microphone", "Fixture Line Input" };
    const int profiles[] = {0, 1};
    const int device = index < 2 ? 0 : 1;
    struct spa_pod_frame frame;
    spa_pod_builder_push_object(builder, &frame, SPA_TYPE_OBJECT_ParamRoute, id);
    spa_pod_builder_add(builder,
        SPA_PARAM_ROUTE_index, SPA_POD_Int(index),
        SPA_PARAM_ROUTE_direction, SPA_POD_Id(index < 2 ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT),
        SPA_PARAM_ROUTE_name, SPA_POD_String(names[index]),
        SPA_PARAM_ROUTE_description, SPA_POD_String(descriptions[index]),
        SPA_PARAM_ROUTE_priority, SPA_POD_Int(100),
        SPA_PARAM_ROUTE_available, SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes),
        SPA_PARAM_ROUTE_profiles, SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 2, profiles),
        SPA_PARAM_ROUTE_devices, SPA_POD_Array(sizeof(int), SPA_TYPE_Int, 1, &device), 0);
    if (id == SPA_PARAM_Route) {
        spa_pod_builder_add(builder,
            SPA_PARAM_ROUTE_device, SPA_POD_Int(device),
            SPA_PARAM_ROUTE_profile, SPA_POD_Int(self->active_profile),
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
    const uint32_t length = id == SPA_PARAM_EnumRoute ? 4 :
        id == SPA_PARAM_EnumProfile ? (self->output_profile_variant ? 4 : 2) :
        id == SPA_PARAM_Route ? 2 : 1;
    uint32_t emitted = 0;
    for (uint32_t index = start; index < length && emitted < maximum; ++index) {
        uint8_t buffer[4096];
        struct spa_pod_builder builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        struct spa_pod *pod;
        if (id == SPA_PARAM_EnumProfile || id == SPA_PARAM_Profile)
            pod = profile(self, &builder, id, id == SPA_PARAM_Profile ? self->active_profile : (int) index);
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
    if (id == SPA_PARAM_Profile) {
        int index = -1;
        bool save = self->save_profile;
        if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, NULL,
                SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
                SPA_PARAM_PROFILE_save, SPA_POD_OPT_Bool(&save)) < 0 || index < 0 ||
                index > (self->output_profile_variant ? 3 : 1))
            return -EINVAL;
        if (index != self->active_profile || save != self->save_profile) {
            const bool profile_changed = index != self->active_profile;
            self->active_profile = index;
            self->save_profile = save;
            if (self->output_profile_variant && index == 1) {
                self->active_routes[1] = 3;
                self->volumes[1][0] = 0.4f;
                self->volumes[1][1] = 0.4f;
                self->mute[1] = true;
            }
            self->params[1].flags ^= SPA_PARAM_INFO_SERIAL;
            self->params[3].flags ^= SPA_PARAM_INFO_SERIAL;
            emit_info(self);
            if (profile_changed && self->recreate_output_on_profile_change) {
                if (self->optical_monitor) {
                    spa_hook_remove(&self->optical_monitor_listener);
                    pw_stream_destroy(self->optical_monitor);
                    self->optical_monitor = NULL;
                    self->optical_pcm_frames = 0;
                    self->optical_sample_printed = false;
                    self->optical_format_printed = false;
                }
                if (self->nodes[0]) pw_proxy_destroy((struct pw_proxy *)self->nodes[0]);
                self->nodes[0] = NULL;
                create_output_node(self);
                if (index == 1) create_optical_monitor(self);
            }
        }
        printf("profile-set index=%d save=%d\n", index, save);
        fflush(stdout);
        return 0;
    }
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
    struct fixture *self = data;
    if (number == SIGUSR1) {
        self->hold_output_node = true;
        printf("output-node-hold-ready\n");
        fflush(stdout);
        return;
    }
    pw_main_loop_quit(self->loop);
}

static void card_bound(void *data, uint32_t id) {
    struct fixture *self = data;
    self->card_id = id;
    if (self->recreate_output_on_profile_change && self->active_profile >= 0) create_output_node(self);
    char card[32];
    snprintf(card, sizeof(card), "%u", id);
    for (unsigned direction = self->recreate_output_on_profile_change ? 1U : 0U;
         direction < 2; ++direction) {
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
    self.output_profile_variant = getenv("PIPETUNE_PROFILE_FIXTURE") != NULL;
    self.recreate_output_on_profile_change = getenv("PIPETUNE_PROFILE_RECREATE") != NULL;
    self.omit_profile_classes = getenv("PIPETUNE_PROFILE_NO_CLASSES") != NULL;
    self.monitor_optical_pcm = getenv("PIPETUNE_PROFILE_MONITOR") != NULL;
    self.device.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Device, SPA_VERSION_DEVICE, &methods, &self);
    spa_hook_list_init(&self.listeners);
    self.loop = pw_main_loop_new(NULL);
    struct pw_loop *loop = pw_main_loop_get_loop(self.loop);
    struct spa_source *signals[] = {
        pw_loop_add_signal(loop, SIGTERM, on_signal, &self),
        pw_loop_add_signal(loop, SIGINT, on_signal, &self),
        pw_loop_add_signal(loop, SIGUSR1, on_signal, &self),
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
    if (self.optical_monitor) {
        spa_hook_remove(&self.optical_monitor_listener);
        pw_stream_destroy(self.optical_monitor);
    }
    for (unsigned i = 0; i < 2; ++i) if (self.nodes[i]) pw_proxy_destroy((struct pw_proxy *)self.nodes[i]);
    spa_hook_remove(&self.bound_listener);
    pw_proxy_destroy(proxy);
    pw_core_disconnect(core);
    pw_context_destroy(context);
    for (unsigned i = 0; i < SPA_N_ELEMENTS(signals); ++i) pw_loop_destroy_source(loop, signals[i]);
    pw_main_loop_destroy(self.loop);
    pw_deinit();
    return 0;
}
