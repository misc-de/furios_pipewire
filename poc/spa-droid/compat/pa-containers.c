/* Replacement for the pulsecore containers the ported droid code uses.
 * The collections here are small (profiles, mappings, ports), so these are
 * deliberately plain implementations rather than reproductions of the PA
 * originals. */
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "pulse/xmalloc.h"
#include "pulsecore/core-util.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"
#include "pulsecore/mutex.h"
#include "pulsecore/shared.h"
#include "pulsecore/strlist.h"
#include "pulsecore/strbuf.h"

/* ---------------- hash and comparison functions ---------------- */

unsigned pa_idxset_string_hash_func(const void *p) {
    unsigned h = 5381;
    const char *c;
    for (c = p; *c; c++) h = (h << 5) + h + (unsigned char) *c;
    return h;
}
int pa_idxset_string_compare_func(const void *a, const void *b) { return strcmp(a, b); }

unsigned pa_idxset_trivial_hash_func(const void *p) {
    return (unsigned) (((uintptr_t) p) >> 4);
}
int pa_idxset_trivial_compare_func(const void *a, const void *b) {
    return a == b ? 0 : (a < b ? -1 : 1);
}

/* ---------------- pa_hashmap ---------------- */

#define HASHMAP_BUCKETS 31

struct hm_entry { void *key, *value; struct hm_entry *next; };

struct pa_hashmap {
    struct hm_entry *buckets[HASHMAP_BUCKETS];
    pa_hash_func_t hash;
    pa_compare_func_t compare;
    pa_free_cb_t key_free, value_free;
    unsigned n;
};

pa_hashmap *pa_hashmap_new_full(pa_hash_func_t hash_func, pa_compare_func_t compare_func,
                                pa_free_cb_t key_free_cb, pa_free_cb_t value_free_cb) {
    pa_hashmap *h = pa_xnew0(pa_hashmap, 1);
    h->hash = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    h->compare = compare_func ? compare_func : pa_idxset_trivial_compare_func;
    h->key_free = key_free_cb;
    h->value_free = value_free_cb;
    return h;
}

pa_hashmap *pa_hashmap_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func) {
    return pa_hashmap_new_full(hash_func, compare_func, NULL, NULL);
}

void pa_hashmap_free(pa_hashmap *h) {
    unsigned i;
    if (!h) return;
    for (i = 0; i < HASHMAP_BUCKETS; i++) {
        struct hm_entry *e = h->buckets[i];
        while (e) {
            struct hm_entry *next = e->next;
            if (h->key_free) h->key_free(e->key);
            if (h->value_free) h->value_free(e->value);
            pa_xfree(e);
            e = next;
        }
    }
    pa_xfree(h);
}

static struct hm_entry *hm_find(const pa_hashmap *h, const void *key, unsigned *bucket) {
    struct hm_entry *e;
    *bucket = h->hash(key) % HASHMAP_BUCKETS;
    for (e = h->buckets[*bucket]; e; e = e->next)
        if (h->compare(e->key, key) == 0) return e;
    return NULL;
}

int pa_hashmap_put(pa_hashmap *h, void *key, void *value) {
    unsigned b;
    struct hm_entry *e;
    if (!h) return -1;
    if (hm_find(h, key, &b)) return -1;      /* Schluessel existiert bereits */
    e = pa_xnew0(struct hm_entry, 1);
    e->key = key; e->value = value;
    e->next = h->buckets[b];
    h->buckets[b] = e;
    h->n++;
    return 0;
}

void *pa_hashmap_get(const pa_hashmap *h, const void *key) {
    unsigned b;
    struct hm_entry *e;
    if (!h) return NULL;
    e = hm_find(h, key, &b);
    return e ? e->value : NULL;
}

void *pa_hashmap_remove(pa_hashmap *h, const void *key) {
    unsigned b;
    struct hm_entry **pp;
    if (!h) return NULL;
    b = h->hash(key) % HASHMAP_BUCKETS;
    for (pp = &h->buckets[b]; *pp; pp = &(*pp)->next) {
        if (h->compare((*pp)->key, key) == 0) {
            struct hm_entry *e = *pp;
            void *v = e->value;
            *pp = e->next;
            if (h->key_free) h->key_free(e->key);
            pa_xfree(e);
            h->n--;
            return v;
        }
    }
    return NULL;
}

unsigned pa_hashmap_size(const pa_hashmap *h) { return h ? h->n : 0; }

void *pa_hashmap_iterate(const pa_hashmap *h, void **state, const void **key) {
    struct hm_entry *e = *state;
    unsigned b = 0;
    if (!h) return NULL;
    if (e) {
        if (e->next) { e = e->next; goto found; }
        b = h->hash(e->key) % HASHMAP_BUCKETS + 1;
    }
    for (; b < HASHMAP_BUCKETS; b++)
        if ((e = h->buckets[b])) goto found;
    *state = NULL;
    return NULL;
found:
    *state = e;
    if (key) *key = e->key;
    return e->value;
}

/* ---------------- pa_idxset ---------------- */

struct idx_entry { uint32_t idx; void *data; };

struct pa_idxset {
    struct idx_entry *entries;
    unsigned n, alloc;
    uint32_t next_idx;
    pa_hash_func_t hash;
    pa_compare_func_t compare;
};

pa_idxset *pa_idxset_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func) {
    pa_idxset *s = pa_xnew0(pa_idxset, 1);
    s->hash = hash_func ? hash_func : pa_idxset_trivial_hash_func;
    s->compare = compare_func ? compare_func : pa_idxset_trivial_compare_func;
    return s;
}

void pa_idxset_free(pa_idxset *s, pa_free_cb_t free_cb) {
    unsigned i;
    if (!s) return;
    if (free_cb)
        for (i = 0; i < s->n; i++) free_cb(s->entries[i].data);
    pa_xfree(s->entries);
    pa_xfree(s);
}

int pa_idxset_put(pa_idxset *s, void *p, uint32_t *idx) {
    unsigned i;
    if (!s) return -1;
    for (i = 0; i < s->n; i++) {
        if (s->compare(s->entries[i].data, p) == 0) {
            if (idx) *idx = s->entries[i].idx;
            return -1;
        }
    }
    if (s->n == s->alloc) {
        s->alloc = s->alloc ? s->alloc * 2 : 8;
        s->entries = pa_xrealloc(s->entries, sizeof(struct idx_entry) * s->alloc);
    }
    s->entries[s->n].idx = s->next_idx;
    s->entries[s->n].data = p;
    if (idx) *idx = s->next_idx;
    s->next_idx++;
    s->n++;
    return 0;
}

void *pa_idxset_get_by_data(pa_idxset *s, const void *p, uint32_t *idx) {
    unsigned i;
    if (!s) return NULL;
    for (i = 0; i < s->n; i++)
        if (s->compare(s->entries[i].data, p) == 0) {
            if (idx) *idx = s->entries[i].idx;
            return s->entries[i].data;
        }
    return NULL;
}

void *pa_idxset_remove_by_data(pa_idxset *s, const void *p, uint32_t *idx) {
    unsigned i;
    if (!s) return NULL;
    for (i = 0; i < s->n; i++) {
        if (s->compare(s->entries[i].data, p) == 0) {
            void *data = s->entries[i].data;
            if (idx) *idx = s->entries[i].idx;
            memmove(&s->entries[i], &s->entries[i + 1],
                    sizeof(struct idx_entry) * (s->n - i - 1));
            s->n--;
            return data;
        }
    }
    return NULL;
}

unsigned pa_idxset_size(pa_idxset *s) { return s ? s->n : 0; }
bool pa_idxset_isempty(pa_idxset *s) { return !s || s->n == 0; }

/* Entries are kept in ascending idx order; removal preserves that order. */
void *pa_idxset_first(pa_idxset *s, uint32_t *idx) {
    if (!s || s->n == 0) { if (idx) *idx = PA_IDXSET_INVALID; return NULL; }
    if (idx) *idx = s->entries[0].idx;
    return s->entries[0].data;
}

void *pa_idxset_next(pa_idxset *s, uint32_t *idx) {
    unsigned i;
    if (!s || !idx) return NULL;
    for (i = 0; i < s->n; i++) {
        if (s->entries[i].idx > *idx) {
            *idx = s->entries[i].idx;
            return s->entries[i].data;
        }
    }
    *idx = PA_IDXSET_INVALID;
    return NULL;
}

/* ---------------- pa_mutex ---------------- */

struct pa_mutex { pthread_mutex_t m; };

pa_mutex *pa_mutex_new(bool recursive, bool inherit_priority) {
    pa_mutex *m = pa_xnew0(pa_mutex, 1);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    if (recursive) pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&m->m, &attr);
    pthread_mutexattr_destroy(&attr);
    return m;
}
void pa_mutex_free(pa_mutex *m)  { if (!m) return; pthread_mutex_destroy(&m->m); pa_xfree(m); }
void pa_mutex_lock(pa_mutex *m)  { pthread_mutex_lock(&m->m); }
void pa_mutex_unlock(pa_mutex *m){ pthread_mutex_unlock(&m->m); }
bool pa_mutex_try_lock(pa_mutex *m) { return pthread_mutex_trylock(&m->m) == 0; }

/* ---------------- pa_shared: prozessweite Registry ---------------- */

static pa_hashmap *shared_map;

static pa_hashmap *shared_get_map(void) {
    if (!shared_map)
        shared_map = pa_hashmap_new_full(pa_idxset_string_hash_func,
                                         pa_idxset_string_compare_func,
                                         pa_xfree, NULL);
    return shared_map;
}

void *pa_shared_get(pa_core *c, const char *name) {
    (void) c;
    return pa_hashmap_get(shared_get_map(), name);
}
int pa_shared_set(pa_core *c, const char *name, void *data) {
    (void) c;
    return pa_hashmap_put(shared_get_map(), pa_xstrdup(name), data);
}
int pa_shared_remove(pa_core *c, const char *name) {
    (void) c;
    return pa_hashmap_remove(shared_get_map(), name) ? 0 : -1;
}

/* ---------------- pa_strlist ---------------- */

struct pa_strlist { struct pa_strlist *next; char *s; };

pa_strlist *pa_strlist_prepend(pa_strlist *l, const char *s) {
    pa_strlist *n = pa_xnew0(pa_strlist, 1);
    n->s = pa_xstrdup(s);
    n->next = l;
    return n;
}
char *pa_strlist_to_string(pa_strlist *l) {
    pa_strbuf *b = pa_strbuf_new();
    bool first = true;
    for (; l; l = l->next) {
        if (!first) pa_strbuf_puts(b, " ");
        pa_strbuf_puts(b, l->s);
        first = false;
    }
    return pa_strbuf_to_string_free(b);
}
void pa_strlist_free(pa_strlist *l) {
    while (l) { pa_strlist *n = l->next; pa_xfree(l->s); pa_xfree(l); l = n; }
}

/* ---------------- pa_core-Singleton ---------------- */
struct pa_core { int placeholder; };
static struct pa_core compat_core;
pa_core *pa_compat_core(void) { return &compat_core; }
