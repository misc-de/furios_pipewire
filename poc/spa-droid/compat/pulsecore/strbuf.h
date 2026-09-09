#pragma once
typedef struct pa_strbuf pa_strbuf;
pa_strbuf *pa_strbuf_new(void);
void pa_strbuf_free(pa_strbuf *b);
char *pa_strbuf_to_string_free(pa_strbuf *b);
void pa_strbuf_puts(pa_strbuf *b, const char *t);
void pa_strbuf_putsn(pa_strbuf *b, const char *t, size_t l);
