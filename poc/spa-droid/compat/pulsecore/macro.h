#pragma once
#include <assert.h>
#include <stdint.h>
#include "pulsecore/pa-fwd.h"
#include <stdbool.h>
#include <stddef.h>
#define pa_assert(x)              assert(x)
#define pa_assert_se(x)           do { if (!(x)) abort(); } while (0)
#define pa_assert_not_reached()   abort()
#define pa_assert_fp(x)           assert(x)
#define pa_return_if_fail(x)      do { if (!(x)) return; } while (0)
#define pa_return_val_if_fail(x,v) do { if (!(x)) return (v); } while (0)
#ifndef PA_LIKELY
#define PA_LIKELY(x)              __builtin_expect(!!(x),1)
#endif
#ifndef PA_UNLIKELY
#define PA_UNLIKELY(x)            __builtin_expect(!!(x),0)
#endif
#ifndef PA_ELEMENTSOF
#define PA_ELEMENTSOF(x)          (sizeof(x)/sizeof((x)[0]))
#endif
#ifndef PA_MIN
#define PA_MIN(a,b)               ((a)<(b)?(a):(b))
#endif
#ifndef PA_MAX
#define PA_MAX(a,b)               ((a)>(b)?(a):(b))
#endif
#ifndef PA_CLAMP
#define PA_CLAMP(v,lo,hi)         PA_MIN(PA_MAX(v,lo),hi)
#endif
#ifndef PA_GCC_UNUSED
#define PA_GCC_UNUSED             __attribute__((unused))
#endif
#ifndef PA_PRINTF_FUNC
#define PA_PRINTF_FUNC(a,b)       __attribute__((format(printf,a,b)))
#endif
