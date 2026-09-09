#pragma once
#include <stdbool.h>
/* Im SPA-Plugin durch spa_dict ersetzen; hier nur Schnittstelle. */
typedef struct pa_modargs pa_modargs;
pa_modargs *pa_modargs_new(const char *args, const char* const keys[]);
void pa_modargs_free(pa_modargs *ma);
const char *pa_modargs_get_value(pa_modargs *ma, const char *key, const char *def);
int pa_modargs_get_value_boolean(pa_modargs *ma, const char *key, bool *value);
int pa_modargs_get_value_u32(pa_modargs *ma, const char *key, uint32_t *value);
