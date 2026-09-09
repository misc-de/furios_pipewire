#pragma once
#include <stdint.h>
#include "pulsecore/hashmap.h"
#define PA_IDXSET_INVALID ((uint32_t) -1)
typedef struct pa_idxset pa_idxset;

unsigned pa_idxset_string_hash_func(const void *p);
int      pa_idxset_string_compare_func(const void *a, const void *b);
unsigned pa_idxset_trivial_hash_func(const void *p);
int      pa_idxset_trivial_compare_func(const void *a, const void *b);

pa_idxset *pa_idxset_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func);
void  pa_idxset_free(pa_idxset *s, pa_free_cb_t free_cb);
int   pa_idxset_put(pa_idxset *s, void *p, uint32_t *idx);
void *pa_idxset_get_by_data(pa_idxset *s, const void *p, uint32_t *idx);
void *pa_idxset_remove_by_data(pa_idxset *s, const void *p, uint32_t *idx);
unsigned pa_idxset_size(pa_idxset *s);
bool  pa_idxset_isempty(pa_idxset *s);
void *pa_idxset_first(pa_idxset *s, uint32_t *idx);
void *pa_idxset_next(pa_idxset *s, uint32_t *idx);
#define PA_IDXSET_FOREACH(e, s, idx) \
    for ((idx) = 0, (e) = pa_idxset_first((s), &(idx)); (e); (e) = pa_idxset_next((s), &(idx)))
