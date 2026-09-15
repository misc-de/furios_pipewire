/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
#include <stdbool.h>
/* Replaced by spa_dict in the SPA plugin; interface only here. */
typedef struct pa_modargs pa_modargs;
pa_modargs *pa_modargs_new(const char *args, const char* const keys[]);
void pa_modargs_free(pa_modargs *ma);
const char *pa_modargs_get_value(pa_modargs *ma, const char *key, const char *def);
int pa_modargs_get_value_boolean(pa_modargs *ma, const char *key, bool *value);
int pa_modargs_get_value_u32(pa_modargs *ma, const char *key, uint32_t *value);
