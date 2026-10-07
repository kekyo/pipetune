/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <pipewire/pipewire.h>
#include <spa/param/route.h>
#include <spa/pod/parser.h>

struct probe {
    struct pw_main_loop *loop;
    struct pw_core *core;
    struct pw_device *device;
    int sync;
    unsigned count, batch_count, next;
    bool started, failed;
};

static void param(void *data, int seq, uint32_t id, uint32_t index,
        uint32_t next, const struct spa_pod *param) {
    (void)seq;
    (void)index;
    struct probe *self = data;
    uint32_t direction = SPA_ID_INVALID;
    if (id != SPA_PARAM_EnumRoute ||
        spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
            SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction)) < 0 ||
        direction != SPA_DIRECTION_INPUT) self->failed = true;
    ++self->batch_count;
    ++self->count;
    self->next = next;
}

static void done(void *data, uint32_t id, int seq) {
    struct probe *self = data;
    if (id != PW_ID_CORE || seq != self->sync) return;
    if (self->batch_count > 1) self->failed = true;
    if (self->started && (self->batch_count == 0 || self->failed || self->count > 4)) {
        pw_main_loop_quit(self->loop);
        return;
    }
    self->started = true;
    self->batch_count = 0;
    pw_device_enum_params(self->device, 0, SPA_PARAM_EnumRoute, self->next, 1, NULL);
    self->sync = pw_core_sync(self->core, PW_ID_CORE, 0);
}

static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS, .param = param,
};
static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS, .done = done,
};

int main(int argc, char **argv) {
    if (argc != 2) return 2;
    const uint32_t id = (uint32_t) strtoul(argv[1], NULL, 10);
    pw_init(&argc, &argv);
    struct probe self = {0};
    self.loop = pw_main_loop_new(NULL);
    struct pw_context *context = pw_context_new(pw_main_loop_get_loop(self.loop), NULL, 0);
    self.core = pw_context_connect(context, NULL, 0);
    if (!self.core) return 2;
    struct pw_registry *registry = pw_core_get_registry(self.core, PW_VERSION_REGISTRY, 0);
    self.device = pw_registry_bind(registry, id, PW_TYPE_INTERFACE_Device, PW_VERSION_DEVICE, 0);
    struct spa_hook core_listener = {0}, device_listener = {0};
    pw_device_add_listener(self.device, &device_listener, &device_events, &self);
    pw_core_add_listener(self.core, &core_listener, &core_events, &self);
    self.sync = pw_core_sync(self.core, PW_ID_CORE, 0);
    pw_main_loop_run(self.loop);
    spa_hook_remove(&device_listener);
    spa_hook_remove(&core_listener);
    pw_proxy_destroy((struct pw_proxy *) self.device);
    pw_proxy_destroy((struct pw_proxy *) registry);
    pw_core_disconnect(self.core);
    pw_context_destroy(context);
    pw_main_loop_destroy(self.loop);
    pw_deinit();
    printf("%u input routes\n", self.count);
    return self.failed || self.count != 2 ? 1 : 0;
}
