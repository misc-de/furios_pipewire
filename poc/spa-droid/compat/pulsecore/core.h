#pragma once
/* pa_core is only passed around by the ported code as an opaque pointer
 * (registry key). In the SPA plugin it is NULL. */
#include <pulse/proplist.h>
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"

/* The ported code uses pa_core purely as a registry key and assert target -
 * demonstrably zero dereferences. A process-wide singleton is therefore
 * enough and keeps the pa_assert(core) happy. */
pa_core *pa_compat_core(void);
