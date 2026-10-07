/* pipetune - Engine and User Interface for Applied EffeTune DSP on a Linux Desktop
 * Copyright (c) Kouji Matsui. (@kekyo@mi.kekyo.net)
 * Under MIT.
 * https://github.com/kekyo/pipetune/
 */
#include <pulse/pulseaudio.h>
#include <stdio.h>
#include <stdbool.h>

struct probe {
    pa_mainloop *loop;
    pa_context *context;
    pa_stream *stream;
    bool queried;
};

static void fail(struct probe *self) {
    fprintf(stderr, "%s\n", pa_strerror(pa_context_errno(self->context)));
    pa_mainloop_quit(self->loop, 1);
}

static void sinks(pa_context *context, const pa_sink_info *info, int eol, void *data) {
    (void) context;
    struct probe *self = data;
    if (eol < 0) fail(self);
    else if (eol > 0) pa_mainloop_quit(self->loop, 0);
    else if (info) printf("%s\n", info->name);
}

static void recorded(pa_stream *stream, size_t bytes, void *data) {
    (void) bytes;
    struct probe *self = data;
    const void *samples;
    size_t length;
    if (pa_stream_peek(stream, &samples, &length) < 0) {
        fail(self);
        return;
    }
    if (length == 0) return;
    pa_stream_drop(stream);
    if (self->queried) return;
    /* GNOME keeps an input peak meter in the same context as its device list.
     * Query after PCM arrives, so the recording client already owns a linked
     * stream and the permission policy has handled it. */
    self->queried = true;
    pa_operation *operation = pa_context_get_sink_info_list(self->context, sinks, self);
    if (!operation) fail(self);
    else pa_operation_unref(operation);
}

static void stream_state(pa_stream *stream, void *data) {
    if (pa_stream_get_state(stream) == PA_STREAM_FAILED) fail(data);
}

static void context_state(pa_context *context, void *data) {
    struct probe *self = data;
    if (pa_context_get_state(context) == PA_CONTEXT_FAILED) {
        fail(self);
        return;
    }
    if (pa_context_get_state(context) != PA_CONTEXT_READY) return;
    const pa_sample_spec format = {PA_SAMPLE_FLOAT32NE, 25, 1};
    self->stream = pa_stream_new(context, "Desktop input peak meter", &format, NULL);
    if (!self->stream) {
        fail(self);
        return;
    }
    pa_stream_set_state_callback(self->stream, stream_state, self);
    pa_stream_set_read_callback(self->stream, recorded, self);
    const pa_buffer_attr attributes = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX, sizeof(float)};
    if (pa_stream_connect_record(self->stream, "fixture.input", &attributes,
            PA_STREAM_PEAK_DETECT | PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY) < 0)
        fail(self);
}

int main(void) {
    struct probe self = {0};
    self.loop = pa_mainloop_new();
    if (!self.loop) return 1;
    self.context = pa_context_new(pa_mainloop_get_api(self.loop), "Desktop output selector");
    if (!self.context) {
        pa_mainloop_free(self.loop);
        return 1;
    }
    pa_context_set_state_callback(self.context, context_state, &self);
    int result = 1;
    if (pa_context_connect(self.context, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL) >= 0)
        pa_mainloop_run(self.loop, &result);
    if (self.stream) {
        pa_stream_set_state_callback(self.stream, NULL, NULL);
        pa_stream_disconnect(self.stream);
        pa_stream_unref(self.stream);
    }
    pa_context_disconnect(self.context);
    pa_context_unref(self.context);
    pa_mainloop_free(self.loop);
    return result;
}
