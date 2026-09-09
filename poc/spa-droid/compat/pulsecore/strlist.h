#pragma once
typedef struct pa_strlist pa_strlist;
pa_strlist *pa_strlist_prepend(pa_strlist *l, const char *s);
char *pa_strlist_to_string(pa_strlist *l);
void pa_strlist_free(pa_strlist *l);
