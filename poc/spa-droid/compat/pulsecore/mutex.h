#pragma once
#include <stdbool.h>
typedef struct pa_mutex pa_mutex;
pa_mutex *pa_mutex_new(bool recursive, bool inherit_priority);
void pa_mutex_free(pa_mutex *m);
void pa_mutex_lock(pa_mutex *m);
bool pa_mutex_try_lock(pa_mutex *m);
void pa_mutex_unlock(pa_mutex *m);
