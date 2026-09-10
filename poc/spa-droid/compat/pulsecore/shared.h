#pragma once
/* String -> pointer registry. In PulseAudio it hangs off pa_core; here it is
 * a process-wide map, since we have exactly one HAL. */
typedef struct pa_core pa_core;
void *pa_shared_get(pa_core *c, const char *name);
int   pa_shared_set(pa_core *c, const char *name, void *data);
int   pa_shared_remove(pa_core *c, const char *name);
