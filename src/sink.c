#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>

#include "sink.h"
#include "sinkinput.h"
#include "namereg.h"
#include "util.h"
#include "sample-util.h"

#define MAX_MIX_CHANNELS 32

struct pa_sink* pa_sink_new(struct pa_core *core, const char *name, int fail, const struct pa_sample_spec *spec) {
    struct pa_sink *s;
    char *n = NULL;
    char st[256];
    int r;
    assert(core && name && spec);

    s = malloc(sizeof(struct pa_sink));
    assert(s);

    if (!(name = pa_namereg_register(core, name, PA_NAMEREG_SINK, s, fail))) {
        free(s);
        return NULL;
    }
    
    s->name = strdup(name);
    s->description = NULL;
    
    s->owner = NULL;
    s->core = core;
    s->sample_spec = *spec;
    s->inputs = pa_idxset_new(NULL, NULL);

    n = pa_sprintf_malloc("%s_monitor", name);
    s->monitor_source = pa_source_new(core, n, 0, spec);
    assert(s->monitor_source);
    free(n);
    s->monitor_source->monitor_of = s;
    
    s->volume = PA_VOLUME_NORM;

    s->notify = NULL;
    s->get_latency = NULL;
    s->userdata = NULL;

    r = pa_idxset_put(core->sinks, s, &s->index);
    assert(s->index != PA_IDXSET_INVALID && r >= 0);
    
    pa_sample_snprint(st, sizeof(st), spec);
    fprintf(stderr, "sink: created %u \"%s\" with sample spec \"%s\"\n", s->index, s->name, st);
    
    return s;
}

void pa_sink_free(struct pa_sink *s) {
    struct pa_sink_input *i, *j = NULL;
    assert(s);

    pa_namereg_unregister(s->core, s->name);
    
    while ((i = pa_idxset_first(s->inputs, NULL))) {
        assert(i != j);
        pa_sink_input_kill(i);
        j = i;
    }
    pa_idxset_free(s->inputs, NULL, NULL);

    pa_source_free(s->monitor_source);
    pa_idxset_remove_by_data(s->core->sinks, s, NULL);

    fprintf(stderr, "sink: freed %u \"%s\"\n", s->index, s->name);
    
    free(s->name);
    free(s->description);
    free(s);
}

void pa_sink_notify(struct pa_sink*s) {
    assert(s);

    if (s->notify)
        s->notify(s);
}

static unsigned fill_mix_info(struct pa_sink *s, struct pa_mix_info *info, unsigned maxinfo) {
    uint32_t index = PA_IDXSET_INVALID;
    struct pa_sink_input *i;
    unsigned n = 0;
    
    assert(s && info);

    for (i = pa_idxset_first(s->inputs, &index); maxinfo > 0 && i; i = pa_idxset_next(s->inputs, &index)) {
        if (pa_sink_input_peek(i, &info->chunk) < 0)
            continue;

        info->volume = i->volume;
        
        assert(info->chunk.memblock && info->chunk.memblock->data && info->chunk.length);
        info->userdata = i;
        
        info++;
        maxinfo--;
        n++;
    }

    return n;
}

static void inputs_drop(struct pa_sink *s, struct pa_mix_info *info, unsigned maxinfo, size_t length) {
    assert(s && info);
    
    for (; maxinfo > 0; maxinfo--, info++) {
        struct pa_sink_input *i = info->userdata;
        assert(i && info->chunk.memblock);
        
        pa_memblock_unref(info->chunk.memblock);
        pa_sink_input_drop(i, length);
    }
}
        
int pa_sink_render(struct pa_sink*s, size_t length, struct pa_memchunk *result) {
    struct pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t l;
    assert(s && length && result);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        return -1;

    if (n == 1) {
        uint32_t volume = PA_VOLUME_NORM;
        struct pa_sink_input *i = info[0].userdata;
        assert(i);
        *result = info[0].chunk;
        pa_memblock_ref(result->memblock);

        if (result->length > length)
            result->length = length;

        l = result->length;

        if (s->volume != PA_VOLUME_NORM || info[0].volume != PA_VOLUME_NORM)
            volume = pa_volume_multiply(s->volume, info[0].volume);
        
        if (volume != PA_VOLUME_NORM) {
            pa_memchunk_make_writable(result);
            pa_volume_memchunk(result, &s->sample_spec, volume);
        }
    } else {
        result->memblock = pa_memblock_new(length);
        assert(result->memblock);

        result->length = l = pa_mix(info, n, result->memblock->data, length, &s->sample_spec, s->volume);
        result->index = 0;
        
        assert(l);
    }

    inputs_drop(s, info, n, l);

    assert(s->monitor_source);
    pa_source_post(s->monitor_source, result);

    return 0;
}

int pa_sink_render_into(struct pa_sink*s, struct pa_memchunk *target) {
    struct pa_mix_info info[MAX_MIX_CHANNELS];
    unsigned n;
    size_t l;
    assert(s && target && target->length && target->memblock && target->memblock->data);
    
    n = fill_mix_info(s, info, MAX_MIX_CHANNELS);

    if (n <= 0)
        return -1;

    if (n == 1) {
        uint32_t volume = PA_VOLUME_NORM;
        struct pa_sink_info *i = info[0].userdata;
        assert(i);

        l = target->length;
        if (l > info[0].chunk.length)
            l = info[0].chunk.length;
        
        memcpy(target->memblock->data+target->index, info[0].chunk.memblock->data + info[0].chunk.index, l);
        target->length = l;

        if (s->volume != PA_VOLUME_NORM || info[0].volume != PA_VOLUME_NORM)
            volume = pa_volume_multiply(s->volume, info[0].volume);

        if (volume != PA_VOLUME_NORM)
            pa_volume_memchunk(target, &s->sample_spec, volume);
    } else
        target->length = l = pa_mix(info, n, target->memblock->data+target->index, target->length, &s->sample_spec, s->volume);
    
    assert(l);
    inputs_drop(s, info, n, l);

    assert(s->monitor_source);
    pa_source_post(s->monitor_source, target);

    return 0;
}

void pa_sink_render_into_full(struct pa_sink *s, struct pa_memchunk *target) {
    struct pa_memchunk chunk;
    size_t l, d;
    assert(s && target && target->memblock && target->length && target->memblock->data);

    l = target->length;
    d = 0;
    while (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        
        if (pa_sink_render_into(s, &chunk) < 0)
            break;

        d += chunk.length;
        l -= chunk.length;
    }

    if (l > 0) {
        chunk = *target;
        chunk.index += d;
        chunk.length -= d;
        pa_silence_memchunk(&chunk, &s->sample_spec);
    }
}

uint32_t pa_sink_get_latency(struct pa_sink *s) {
    assert(s);

    if (!s->get_latency)
        return 0;

    return s->get_latency(s);
}

struct pa_sink* pa_sink_get_default(struct pa_core *c) {
    struct pa_sink *sink;
    assert(c);

    if ((sink = pa_idxset_get_by_index(c->sinks, c->default_sink_index)))
        return sink;

    if (!(sink = pa_idxset_first(c->sinks, &c->default_sink_index)))
        return NULL;

    fprintf(stderr, "core: default sink vanished, setting to %u.\n", sink->index);
    return sink;
}

void pa_sink_set_owner(struct pa_sink *sink, struct pa_module *m) {
    sink->owner = m;

    if (sink->monitor_source)
        pa_source_set_owner(sink->monitor_source, m);
}
