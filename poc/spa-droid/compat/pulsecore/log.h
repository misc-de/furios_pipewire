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
#define pa_log(...)  do { fprintf(stderr, __VA_ARGS__); fputc(0x0a, stderr); } while (0)
#define pa_log_debug(...)  do { fprintf(stderr, __VA_ARGS__); fputc(0x0a, stderr); } while (0)
#define pa_log_info(...)  do { fprintf(stderr, __VA_ARGS__); fputc(0x0a, stderr); } while (0)
#define pa_log_warn(...)  do { fprintf(stderr, __VA_ARGS__); fputc(0x0a, stderr); } while (0)
#define pa_log_error(...)  do { fprintf(stderr, __VA_ARGS__); fputc(0x0a, stderr); } while (0)
#define pa_logl(l,...) do { fprintf(stderr, __VA_ARGS__); fputc(0x0a, stderr); } while (0)
