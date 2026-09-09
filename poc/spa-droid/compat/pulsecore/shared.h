#pragma once
/* String -> Pointer Registry. In PulseAudio haengt sie am pa_core;
 * hier eine prozessweite Map, da wir genau einen HAL haben. */
typedef struct pa_core pa_core;
void *pa_shared_get(pa_core *c, const char *name);
int   pa_shared_set(pa_core *c, const char *name, void *data);
int   pa_shared_remove(pa_core *c, const char *name);
