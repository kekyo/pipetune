/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#define _GNU_SOURCE
#include <stdlib.h>
#include <pipewire/impl.h>
#include <spa/param/route.h>
#include <spa/pod/parser.h>

struct module;
/* Context, client and resource listeners are owned by the module. Destruction
 * removes each listener before its state is freed; no real-time callbacks run
 * here and the module never owns or recreates an audio device or node. */
struct client {
    struct spa_list link;
    struct module *module;
    struct pw_impl_client *client;
    struct spa_hook listener;
};
struct resource {
    struct spa_list link;
    struct module *module;
    struct pw_resource *resource;
    struct spa_callbacks original;
    struct spa_hook listener;
    struct pw_device_info *info;
    struct spa_hook method_listener;
    struct spa_hook_list original_methods;
    bool methods_installed;
    int enumeration_seq;
    uint32_t remaining;
    bool enumeration_limited;
    bool serial;
    bool hidden;
};
struct module {
    struct pw_context *context;
    struct spa_hook context_listener;
    struct spa_hook module_listener;
    struct spa_list clients;
    struct spa_list resources;
    unsigned aggregates;
};

static bool filtered(struct resource *self) {
    if (!self->info || !self->info->props ||
        !spa_streq(spa_dict_lookup(self->info->props, PW_KEY_MEDIA_CLASS), "Audio/Device"))
        return false;
    const struct pw_properties *props = pw_impl_client_get_properties(
        pw_resource_get_client(self->resource));
    const char *binary = pw_properties_get(props, PW_KEY_APP_PROCESS_BINARY);
    return self->module->aggregates > 0 &&
        pw_properties_get(props, "wireplumber.daemon") == NULL &&
        !spa_streq(binary, "wireplumber") && !spa_streq(binary, "pipetune");
}

static void send_info(struct resource *self, const struct pw_device_info *info) {
    struct pw_device_info copy = *info;
    struct spa_param_info params[info->n_params > 0 ? info->n_params : 1];
    for (uint32_t i = 0; i < info->n_params; ++i) {
        params[i] = info->params[i];
        if (params[i].id == SPA_PARAM_EnumRoute && self->serial)
            params[i].flags ^= SPA_PARAM_INFO_SERIAL;
    }
    copy.params = params;
    spa_callbacks_call(&self->original, struct pw_device_events, info, 0, &copy);
}

static int subscribe_params(void *data, uint32_t *ids, uint32_t n_ids) {
    struct resource *self = data;
    return spa_hook_list_call(&self->original_methods, struct pw_device_methods,
        subscribe_params, 0, ids, n_ids);
}

static int enum_params(void *data, int seq, uint32_t id, uint32_t start,
        uint32_t num, const struct spa_pod *filter) {
    struct resource *self = data;
    self->enumeration_limited = id == SPA_PARAM_EnumRoute && filtered(self);
    self->enumeration_seq = seq;
    self->remaining = num == 0 ? UINT32_MAX : num;
    /* A hidden route must not consume the caller's page. Keep the original
     * indices and filter, scan the remaining routes, then limit visible replies.
     * PipeWire serializes asynchronous Device requests using the client's busy
     * state, so each resource has only one outstanding explicit enumeration. */
    return spa_hook_list_call(&self->original_methods, struct pw_device_methods,
        enum_params, 0, seq, id, start,
        self->enumeration_limited ? UINT32_MAX : num, filter);
}

static int set_param(void *data, uint32_t id, uint32_t flags, const struct spa_pod *param) {
    struct resource *self = data;
    return spa_hook_list_call(&self->original_methods, struct pw_device_methods,
        set_param, 0, id, flags, param);
}

static const struct pw_device_methods device_methods = {
    PW_VERSION_DEVICE_METHODS, .subscribe_params = subscribe_params,
    .enum_params = enum_params, .set_param = set_param,
};

static void device_info(void *data, const struct pw_device_info *info) {
    struct resource *self = data;
    if (!self->methods_installed) {
        /* The initial info is emitted after the resource implementation has
         * been installed. Keep its hooks intact and delegate every operation. */
        spa_hook_list_isolate(pw_resource_get_object_listeners(self->resource),
            &self->original_methods, &self->method_listener, &device_methods, self);
        self->methods_installed = true;
    }
    self->info = pw_device_info_update(self->info, info);
    self->hidden = filtered(self);
    send_info(self, info);
}

static void device_param(void *data, int seq, uint32_t id, uint32_t index,
        uint32_t next, const struct spa_pod *param) {
    struct resource *self = data;
    uint32_t direction = SPA_ID_INVALID;
    if (id == SPA_PARAM_EnumRoute && filtered(self) && param &&
        spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
            SPA_PARAM_ROUTE_direction, SPA_POD_Id(&direction)) >= 0 &&
        direction == SPA_DIRECTION_OUTPUT) return;
    if (id == SPA_PARAM_EnumRoute && self->enumeration_limited &&
        seq == self->enumeration_seq) {
        if (self->remaining == 0) return;
        --self->remaining;
    }
    spa_callbacks_call(&self->original, struct pw_device_events, param, 0,
        seq, id, index, next, param);
}
static const struct pw_device_events device_events = {
    PW_VERSION_DEVICE_EVENTS, .info = device_info, .param = device_param,
};

static void resource_destroy(void *data) {
    struct resource *self = data;
    ((struct spa_interface *) self->resource)->cb = self->original;
    if (self->methods_installed) {
        spa_hook_remove(&self->method_listener);
        spa_hook_list_join(pw_resource_get_object_listeners(self->resource), &self->original_methods);
    }
    spa_hook_remove(&self->listener);
    spa_list_remove(&self->link);
    pw_device_info_free(self->info);
    free(self);
}
static const struct pw_resource_events resource_events = {
    PW_VERSION_RESOURCE_EVENTS, .destroy = resource_destroy,
};

static void resource_added(void *data, struct pw_resource *resource) {
    struct client *client = data;
    if (!spa_streq(pw_resource_get_type(resource, NULL), PW_TYPE_INTERFACE_Device)) return;
    struct resource *self = calloc(1, sizeof(*self));
    if (!self) return;
    self->module = client->module;
    self->resource = resource;
    /* pw_resource_call's public interface is a SPA interface. Delegate its
     * event callbacks without inspecting the private resource structure. */
    struct spa_interface *interface = (struct spa_interface *) resource;
    self->original = interface->cb;
    interface->cb = SPA_CALLBACKS_INIT(&device_events, self);
    spa_list_append(&self->module->resources, &self->link);
    pw_resource_add_listener(resource, &self->listener, &resource_events, self);
}

static void client_destroy(void *data) {
    struct client *self = data;
    spa_hook_remove(&self->listener);
    spa_list_remove(&self->link);
    free(self);
}
static const struct pw_impl_client_events client_events = {
    PW_VERSION_IMPL_CLIENT_EVENTS,
    .destroy = client_destroy, .resource_added = resource_added,
};

static void check_access(void *data, struct pw_impl_client *client) {
    struct module *module = data;
    struct client *existing;
    spa_list_for_each(existing, &module->clients, link)
        if (existing->client == client) return;
    struct client *self = calloc(1, sizeof(*self));
    if (!self) return;
    self->module = module;
    self->client = client;
    spa_list_append(&module->clients, &self->link);
    pw_impl_client_add_listener(client, &self->listener, &client_events, self);
}

static bool aggregate(struct pw_global *global) {
    if (!pw_global_is_type(global, PW_TYPE_INTERFACE_Node)) return false;
    const struct pw_properties *props = pw_impl_node_get_properties(pw_global_get_object(global));
    return spa_streq(pw_properties_get(props, "node.pipetune.aggregate"), "true");
}

static void refresh(struct module *module) {
    struct resource *resource;
    spa_list_for_each(resource, &module->resources, link) {
        if (!resource->info || resource->hidden == filtered(resource)) continue;
        resource->hidden = filtered(resource);
        resource->serial = !resource->serial;
        struct pw_device_info info = *resource->info;
        info.change_mask = PW_DEVICE_CHANGE_MASK_PARAMS;
        send_info(resource, &info);
    }
}

static void global_added(void *data, struct pw_global *global) {
    struct module *module = data;
    if (aggregate(global) && module->aggregates++ == 0) refresh(module);
}

static void global_removed(void *data, struct pw_global *global) {
    struct module *module = data;
    if (aggregate(global) && --module->aggregates == 0) refresh(module);
}
static const struct pw_context_events context_events = {
    PW_VERSION_CONTEXT_EVENTS, .check_access = check_access,
    .global_added = global_added, .global_removed = global_removed,
};

static void module_destroy(void *data) {
    struct module *self = data;
    spa_hook_remove(&self->context_listener);
    spa_hook_remove(&self->module_listener);
    self->aggregates = 0;
    refresh(self);
    struct resource *resource;
    spa_list_consume(resource, &self->resources, link) resource_destroy(resource);
    struct client *client;
    spa_list_consume(client, &self->clients, link) client_destroy(client);
    free(self);
}
static const struct pw_impl_module_events module_events = {
    PW_VERSION_IMPL_MODULE_EVENTS, .destroy = module_destroy,
};

/**
 * Load client-specific audio route presentation into a PipeWire server.
 * @param module PipeWire-owned module whose lifetime owns all listeners.
 * @param args Unused configuration arguments.
 * @return Zero on success, or a negative errno value on allocation failure.
 * @note The server loads this module before external clients connect. The
 * original Device methods, parameters and PCM graph remain unchanged.
 */
SPA_EXPORT int pipewire__module_init(struct pw_impl_module *module, const char *args) {
    (void) args;
    struct module *self = calloc(1, sizeof(*self));
    if (!self) return -ENOMEM;
    self->context = pw_impl_module_get_context(module);
    spa_list_init(&self->clients);
    spa_list_init(&self->resources);
    pw_context_add_listener(self->context, &self->context_listener, &context_events, self);
    pw_impl_module_add_listener(module, &self->module_listener, &module_events, self);
    return 0;
}
