#pragma once
#include <stdbool.h>
#include "pulsecore/macro.h"
#ifndef PA_FREE_CB_T_DEFINED
#define PA_FREE_CB_T_DEFINED
typedef void (*pa_free_cb_t)(void *p);
#endif
typedef unsigned (*pa_hash_func_t)(const void *p);
typedef int (*pa_compare_func_t)(const void *a, const void *b);
typedef struct pa_hashmap pa_hashmap;

pa_hashmap *pa_hashmap_new(pa_hash_func_t hash_func, pa_compare_func_t compare_func);
pa_hashmap *pa_hashmap_new_full(pa_hash_func_t hash_func, pa_compare_func_t compare_func,
                                pa_free_cb_t key_free_cb, pa_free_cb_t value_free_cb);
void pa_hashmap_free(pa_hashmap *h);
int pa_hashmap_put(pa_hashmap *h, void *key, void *value);
void *pa_hashmap_get(const pa_hashmap *h, const void *key);
void *pa_hashmap_remove(pa_hashmap *h, const void *key);
unsigned pa_hashmap_size(const pa_hashmap *h);
void *pa_hashmap_iterate(const pa_hashmap *h, void **state, const void **key);
#define PA_HASHMAP_FOREACH(e, h, state) \
    for ((state) = NULL, (e) = pa_hashmap_iterate((h), &(state), NULL); (e); \
         (e) = pa_hashmap_iterate((h), &(state), NULL))
