/* Ersatz fuer die letzten 14 Funktionen, fuer die das Plugin bisher gegen
 * libpulse gelinkt war.
 *
 * Ausgerechnet der Stack, der PulseAudio ueberfluessig machen soll, lud dessen
 * Client-Bibliothek - fuer Kleinigkeiten wie "sind zwei Kanalkarten gleich?".
 * Die Kopfdateien von libpulse bleiben in Gebrauch (die Typen muessen ja
 * zusammenpassen), nur der Programmcode kommt jetzt von hier.
 */
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>

#include "pulse/xmalloc.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"

/* ---------------- Abtastformat ---------------- */

size_t pa_sample_size_of_format(pa_sample_format_t f) {
    static const size_t table[PA_SAMPLE_MAX] = {
        [PA_SAMPLE_U8]        = 1,
        [PA_SAMPLE_ALAW]      = 1,
        [PA_SAMPLE_ULAW]      = 1,
        [PA_SAMPLE_S16LE]     = 2,
        [PA_SAMPLE_S16BE]     = 2,
        [PA_SAMPLE_FLOAT32LE] = 4,
        [PA_SAMPLE_FLOAT32BE] = 4,
        [PA_SAMPLE_S32LE]     = 4,
        [PA_SAMPLE_S32BE]     = 4,
        [PA_SAMPLE_S24LE]     = 3,
        [PA_SAMPLE_S24BE]     = 3,
        [PA_SAMPLE_S24_32LE]  = 4,
        [PA_SAMPLE_S24_32BE]  = 4,
    };
    if (f < 0 || f >= PA_SAMPLE_MAX)
        return 0;
    return table[f];
}

size_t pa_sample_size(const pa_sample_spec *spec) {
    return spec ? pa_sample_size_of_format(spec->format) : 0;
}

size_t pa_frame_size(const pa_sample_spec *spec) {
    return spec ? pa_sample_size_of_format(spec->format) * spec->channels : 0;
}

pa_usec_t pa_bytes_to_usec(uint64_t length, const pa_sample_spec *spec) {
    size_t fs;
    if (!spec || spec->rate == 0)
        return 0;
    fs = pa_frame_size(spec);
    if (fs == 0)
        return 0;
    return (pa_usec_t) (((double) length / (double) fs) * 1000000.0 / (double) spec->rate);
}

int pa_sample_spec_valid(const pa_sample_spec *spec) {
    if (!spec)
        return 0;
    if (spec->rate == 0 || spec->rate > PA_RATE_MAX)
        return 0;
    if (spec->channels == 0 || spec->channels > PA_CHANNELS_MAX)
        return 0;
    if (spec->format < 0 || spec->format >= PA_SAMPLE_MAX)
        return 0;
    return 1;
}

int pa_sample_spec_equal(const pa_sample_spec *a, const pa_sample_spec *b) {
    if (a == b)
        return 1;
    if (!a || !b)
        return 0;
    return a->format == b->format && a->rate == b->rate && a->channels == b->channels;
}

const char *pa_sample_format_to_string(pa_sample_format_t f) {
    static const char *const names[PA_SAMPLE_MAX] = {
        [PA_SAMPLE_U8]        = "u8",
        [PA_SAMPLE_ALAW]      = "aLaw",
        [PA_SAMPLE_ULAW]      = "uLaw",
        [PA_SAMPLE_S16LE]     = "s16le",
        [PA_SAMPLE_S16BE]     = "s16be",
        [PA_SAMPLE_FLOAT32LE] = "float32le",
        [PA_SAMPLE_FLOAT32BE] = "float32be",
        [PA_SAMPLE_S32LE]     = "s32le",
        [PA_SAMPLE_S32BE]     = "s32be",
        [PA_SAMPLE_S24LE]     = "s24le",
        [PA_SAMPLE_S24BE]     = "s24be",
        [PA_SAMPLE_S24_32LE]  = "s24-32le",
        [PA_SAMPLE_S24_32BE]  = "s24-32be",
    };
    if (f < 0 || f >= PA_SAMPLE_MAX)
        return NULL;
    return names[f];
}

/* ---------------- Kanalkarte ---------------- */

pa_channel_map *pa_channel_map_init_mono(pa_channel_map *m) {
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->channels = 1;
    m->map[0] = PA_CHANNEL_POSITION_MONO;
    return m;
}

pa_channel_map *pa_channel_map_init_stereo(pa_channel_map *m) {
    if (!m)
        return NULL;
    memset(m, 0, sizeof(*m));
    m->channels = 2;
    m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
    m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
    return m;
}

int pa_channel_map_valid(const pa_channel_map *m) {
    unsigned c;
    if (!m || m->channels == 0 || m->channels > PA_CHANNELS_MAX)
        return 0;
    for (c = 0; c < m->channels; c++)
        if (m->map[c] < 0 || m->map[c] >= PA_CHANNEL_POSITION_MAX)
            return 0;
    return 1;
}

int pa_channel_map_equal(const pa_channel_map *a, const pa_channel_map *b) {
    unsigned c;
    if (a == b)
        return 1;
    if (!a || !b || a->channels != b->channels)
        return 0;
    for (c = 0; c < a->channels; c++)
        if (a->map[c] != b->map[c])
            return 0;
    return 1;
}

/* Nur fuer Protokollausgaben gebraucht - kurz und lesbar genuegt. */
char *pa_channel_map_snprint(char *s, size_t l, const pa_channel_map *m) {
    size_t used = 0;
    unsigned c;

    if (!s || l == 0)
        return s;
    s[0] = '\0';
    if (!m || m->channels == 0) {
        snprintf(s, l, "(invalid)");
        return s;
    }
    for (c = 0; c < m->channels && used + 1 < l; c++) {
        int n = snprintf(s + used, l - used, "%s%d",
                         c > 0 ? "," : "", (int) m->map[c]);
        if (n < 0)
            break;
        used += (size_t) n;
    }
    return s;
}

/* ---------------- Eigenschaftsliste ---------------- */

/* Der portierte Code benutzt davon nur vier Funktionen und legt ausschliesslich
 * Zeichenketten ab (die Android-Audioquelle). Mehr braucht es hier nicht. */
struct pa_proplist {
    pa_hashmap *map;
};

pa_proplist *pa_proplist_new(void) {
    pa_proplist *p = pa_xnew0(pa_proplist, 1);
    p->map = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                 pa_idxset_string_compare_func,
                                 pa_xfree, pa_xfree);
    return p;
}

void pa_proplist_free(pa_proplist *p) {
    if (!p)
        return;
    pa_hashmap_free(p->map);
    pa_xfree(p);
}

int pa_proplist_sets(pa_proplist *p, const char *key, const char *value) {
    if (!p || !key || !value)
        return -1;
    pa_xfree(pa_hashmap_remove(p->map, key));
    return pa_hashmap_put(p->map, pa_xstrdup(key), pa_xstrdup(value));
}

const char *pa_proplist_gets(const pa_proplist *p, const char *key) {
    if (!p || !key)
        return NULL;
    return pa_hashmap_get(p->map, key);
}

/* ---------------- Sonstiges ---------------- */

void pa_msleep(unsigned long t) {
    struct timespec ts = {
        .tv_sec  = (time_t) (t / 1000),
        .tv_nsec = (long) ((t % 1000) * 1000000L),
    };
    nanosleep(&ts, NULL);
}
