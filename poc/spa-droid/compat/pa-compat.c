/* Implementierungen der PulseAudio-Hilfsfunktionen, die der portierte
 * droid-Code aufruft. Ersetzt pulsecore fuer den SPA-Port. */
#include <stdlib.h>
#include <string.h>
#include "pulse/xmalloc.h"
#include "pulsecore/core-util.h"
#include "pulsecore/strbuf.h"
#include "pulsecore/modargs.h"

#define WHITESPACE " \t\n\r"

char *pa_split(const char *c, const char *delimiter, const char **state) {
    const char *current = *state ? *state : c;
    size_t l;
    if (!*current) return NULL;
    l = strcspn(current, delimiter);
    *state = current + l;
    if (**state) (*state)++;
    return pa_xstrndup(current, l);
}

char *pa_split_spaces(const char *c, const char **state) {
    const char *current = *state ? *state : c;
    size_t l;
    if (!*current || *c == 0) return NULL;
    current += strspn(current, WHITESPACE);
    l = strcspn(current, WHITESPACE);
    if (l == 0) return NULL;
    *state = current + l;
    return pa_xstrndup(current, l);
}

/* --- pa_strbuf: einfacher dynamischer Puffer --- */
struct pa_strbuf { char *data; size_t len, alloc; };

pa_strbuf *pa_strbuf_new(void) {
    pa_strbuf *b = pa_xnew0(pa_strbuf, 1);
    b->alloc = 256; b->data = pa_xmalloc(b->alloc); b->data[0] = 0;
    return b;
}
void pa_strbuf_free(pa_strbuf *b) { if (!b) return; pa_xfree(b->data); pa_xfree(b); }

static void strbuf_grow(pa_strbuf *b, size_t need) {
    if (b->len + need + 1 <= b->alloc) return;
    while (b->len + need + 1 > b->alloc) b->alloc *= 2;
    b->data = pa_xrealloc(b->data, b->alloc);
}
void pa_strbuf_putsn(pa_strbuf *b, const char *t, size_t l) {
    if (!t || !l) return;
    strbuf_grow(b, l);
    memcpy(b->data + b->len, t, l);
    b->len += l; b->data[b->len] = 0;
}
void pa_strbuf_puts(pa_strbuf *b, const char *t) { if (t) pa_strbuf_putsn(b, t, strlen(t)); }
char *pa_strbuf_to_string_free(pa_strbuf *b) { char *r = b->data; b->data = NULL; pa_xfree(b); return r; }

char *pa_replace(const char *s, const char *a, const char *b) {
    size_t la;
    pa_strbuf *sb;
    if (!s) return NULL;
    if (!a || !*a) return pa_xstrdup(s);
    la = strlen(a);
    sb = pa_strbuf_new();
    for (;;) {
        const char *p = strstr(s, a);
        if (!p) break;
        pa_strbuf_putsn(sb, s, (size_t)(p - s));
        pa_strbuf_puts(sb, b);
        s = p + la;
    }
    pa_strbuf_puts(sb, s);
    return pa_strbuf_to_string_free(sb);
}

/* --- pa_modargs: minimal, spaeter durch spa_dict ersetzen --- */
struct pa_modargs { char **keys; char **vals; unsigned n; };

pa_modargs *pa_modargs_new(const char *args, const char* const keys[]) {
    pa_modargs *ma = pa_xnew0(pa_modargs, 1);
    const char *state = NULL;
    char *tok;
    (void) keys;
    if (!args) return ma;
    while ((tok = pa_split(args, " ", &state))) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = 0;
            ma->keys = pa_xrealloc(ma->keys, sizeof(char*) * (ma->n + 1));
            ma->vals = pa_xrealloc(ma->vals, sizeof(char*) * (ma->n + 1));
            ma->keys[ma->n] = pa_xstrdup(tok);
            ma->vals[ma->n] = pa_xstrdup(eq + 1);
            ma->n++;
        }
        pa_xfree(tok);
    }
    return ma;
}

void pa_modargs_free(pa_modargs *ma) {
    unsigned i;
    if (!ma) return;
    for (i = 0; i < ma->n; i++) { pa_xfree(ma->keys[i]); pa_xfree(ma->vals[i]); }
    pa_xfree(ma->keys); pa_xfree(ma->vals); pa_xfree(ma);
}

const char *pa_modargs_get_value(pa_modargs *ma, const char *key, const char *def) {
    unsigned i;
    if (!ma) return def;
    for (i = 0; i < ma->n; i++)
        if (pa_streq(ma->keys[i], key)) return ma->vals[i];
    return def;
}

int pa_modargs_get_value_boolean(pa_modargs *ma, const char *key, bool *value) {
    const char *v = pa_modargs_get_value(ma, key, NULL);
    if (!v) return -1;
    if (pa_streq(v, "1") || pa_streq(v, "true") || pa_streq(v, "yes")) { *value = true;  return 0; }
    if (pa_streq(v, "0") || pa_streq(v, "false")|| pa_streq(v, "no"))  { *value = false; return 0; }
    return -1;
}

int pa_modargs_get_value_u32(pa_modargs *ma, const char *key, uint32_t *value) {
    const char *v = pa_modargs_get_value(ma, key, NULL);
    return v ? pa_atou(v, value) : -1;
}
