/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
#include <stdbool.h>
typedef struct pa_mutex pa_mutex;
pa_mutex *pa_mutex_new(bool recursive, bool inherit_priority);
void pa_mutex_free(pa_mutex *m);
void pa_mutex_lock(pa_mutex *m);
bool pa_mutex_try_lock(pa_mutex *m);
void pa_mutex_unlock(pa_mutex *m);
