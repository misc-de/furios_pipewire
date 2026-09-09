#pragma once
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "pulse/xmalloc.h"
#include <stdint.h>
#include "pulsecore/macro.h"
#include "pulsecore/log.h"
static inline bool pa_streq(const char *a,const char *b){ return strcmp(a,b)==0; }
static inline bool pa_safe_streq(const char *a,const char *b){ if(!a||!b) return a==b; return strcmp(a,b)==0; }
static inline bool pa_startswith(const char *s,const char *p){ return strncmp(s,p,strlen(p))==0; }
static inline bool pa_endswith(const char *s,const char *p){ size_t ls=strlen(s),lp=strlen(p); return ls>=lp && strcmp(s+ls-lp,p)==0; }
#define pa_snprintf snprintf
#define pa_vsnprintf vsnprintf
static inline char *pa_sprintf_malloc(const char *fmt,...){
    va_list ap; char *r=NULL; va_start(ap,fmt); if (vasprintf(&r,fmt,ap)<0) abort(); va_end(ap); return r;
}
char *pa_replace(const char *s,const char *a,const char *b);
char *pa_split_spaces(const char *c,const char **state);
static inline void pa_msleep(unsigned ms){ usleep(ms*1000); }

char *pa_split(const char *c, const char *delim, const char **state);
static inline int pa_atoi(const char *s, int32_t *r){ char *e; long v=strtol(s,&e,0); if(!e||*e||e==s) return -1; *r=(int32_t)v; return 0; }
static inline int pa_atou(const char *s, uint32_t *r){ char *e; unsigned long v=strtoul(s,&e,0); if(!e||*e||e==s) return -1; *r=(uint32_t)v; return 0; }
