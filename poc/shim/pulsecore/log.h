#pragma once
#include <stdio.h>
#include <stdint.h>
#include "pulsecore/macro.h"
/* Im spaeteren SPA-Plugin auf spa_log_* umbiegen. */
#define PA_LOG_DEBUG 0
#define PA_LOG_INFO  1
#define PA_LOG_NOTICE 2
#define PA_LOG_WARN  3
#define PA_LOG_ERROR 4
typedef int pa_log_level_t;
#define pa_log(...)        fprintf(stderr, __VA_ARGS__)
#define pa_log_debug(...)  fprintf(stderr, __VA_ARGS__)
#define pa_log_info(...)   fprintf(stderr, __VA_ARGS__)
#define pa_log_warn(...)   fprintf(stderr, __VA_ARGS__)
#define pa_log_error(...)  fprintf(stderr, __VA_ARGS__)
#define pa_logl(l,...)     fprintf(stderr, __VA_ARGS__)
