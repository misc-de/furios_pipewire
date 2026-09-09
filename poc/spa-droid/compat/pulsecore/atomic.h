#pragma once
#include <stdint.h>
typedef struct pa_atomic { volatile int value; } pa_atomic_t;
#define PA_ATOMIC_INIT(v) { .value = (v) }
static inline int  pa_atomic_load(const pa_atomic_t *a) { return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
static inline void pa_atomic_store(pa_atomic_t *a, int i) { __atomic_store_n(&a->value, i, __ATOMIC_SEQ_CST); }
static inline int  pa_atomic_add(pa_atomic_t *a, int i) { return __atomic_fetch_add(&a->value, i, __ATOMIC_SEQ_CST); }
static inline int  pa_atomic_sub(pa_atomic_t *a, int i) { return __atomic_fetch_sub(&a->value, i, __ATOMIC_SEQ_CST); }
static inline int  pa_atomic_inc(pa_atomic_t *a) { return pa_atomic_add(a, 1); }
static inline int  pa_atomic_dec(pa_atomic_t *a) { return pa_atomic_sub(a, 1); }
typedef struct pa_atomic_ptr { volatile void *value; } pa_atomic_ptr_t;
static inline void *pa_atomic_ptr_load(const pa_atomic_ptr_t *a) { return (void*) __atomic_load_n(&a->value, __ATOMIC_SEQ_CST); }
static inline void  pa_atomic_ptr_store(pa_atomic_ptr_t *a, void *p) { __atomic_store_n(&a->value, p, __ATOMIC_SEQ_CST); }
