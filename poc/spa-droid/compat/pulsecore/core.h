#pragma once
/* pa_core wird vom portierten Code nur als undurchsichtiger Zeiger
 * durchgereicht (Registry-Schluessel). Im SPA-Plugin ist er NULL. */
#include <pulse/proplist.h>
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"

/* Der portierte Code nutzt pa_core ausschliesslich als Registry-Schluessel
 * und Assert-Ziel - nachgewiesen null Dereferenzierungen. Ein prozessweites
 * Singleton genuegt daher und haelt die pa_assert(core) zufrieden. */
pa_core *pa_compat_core(void);
