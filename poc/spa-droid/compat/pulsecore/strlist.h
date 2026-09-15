/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
typedef struct pa_strlist pa_strlist;
pa_strlist *pa_strlist_prepend(pa_strlist *l, const char *s);
char *pa_strlist_to_string(pa_strlist *l);
void pa_strlist_free(pa_strlist *l);
