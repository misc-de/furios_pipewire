#pragma once
#include <stdlib.h>
#include <string.h>
static inline void *pa_xmalloc(size_t n){ void *p=malloc(n); if(!p) abort(); return p; }
static inline void *pa_xmalloc0(size_t n){ void *p=calloc(1,n); if(!p) abort(); return p; }
static inline void *pa_xrealloc(void *o,size_t n){ void *p=realloc(o,n); if(!p) abort(); return p; }
static inline void pa_xfree(void *p){ free(p); }
static inline char *pa_xstrdup(const char *s){ return s?strdup(s):NULL; }
static inline char *pa_xstrndup(const char *s,size_t n){ return s?strndup(s,n):NULL; }
static inline void *pa_xmemdup(const void *p,size_t n){ void *q=pa_xmalloc(n); memcpy(q,p,n); return q; }
#define pa_xnew(t,n)     ((t*)pa_xmalloc(sizeof(t)*(n)))
#define pa_xnew0(t,n)    ((t*)pa_xmalloc0(sizeof(t)*(n)))
#define pa_xnewdup(t,p,n)((t*)pa_xmemdup((p),sizeof(t)*(n)))
