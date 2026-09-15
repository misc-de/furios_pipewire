/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
typedef struct pa_strbuf pa_strbuf;
pa_strbuf *pa_strbuf_new(void);
void pa_strbuf_free(pa_strbuf *b);
char *pa_strbuf_to_string_free(pa_strbuf *b);
void pa_strbuf_puts(pa_strbuf *b, const char *t);
void pa_strbuf_putsn(pa_strbuf *b, const char *t, size_t l);
