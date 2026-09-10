#pragma once
/* Types of the PulseAudio graph. In the SPA port they only appear as
 * pointers in signatures; the functions that dereference them are excluded
 * by tools/port-droid-util.py. */
typedef struct pa_core pa_core;
typedef struct pa_sink pa_sink;
typedef struct pa_source pa_source;
typedef struct pa_card pa_card;
typedef struct pa_card_profile pa_card_profile;
typedef struct pa_device_port pa_device_port;
typedef struct pa_hook_slot pa_hook_slot;
typedef enum pa_hook_result { PA_HOOK_OK = 0, PA_HOOK_STOP = 1, PA_HOOK_CANCEL = -1 } pa_hook_result_t;
