/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
/* String -> pointer registry. In PulseAudio it hangs off pa_core; here it is
 * a process-wide map, since we have exactly one HAL. */
typedef struct pa_core pa_core;
void *pa_shared_get(pa_core *c, const char *name);
int   pa_shared_set(pa_core *c, const char *name, void *data);
int   pa_shared_remove(pa_core *c, const char *name);
