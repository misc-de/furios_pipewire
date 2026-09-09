#pragma once
#include <stddef.h>
typedef struct pa_memblock pa_memblock;
typedef struct pa_memchunk { pa_memblock *memblock; size_t index, length; } pa_memchunk;
