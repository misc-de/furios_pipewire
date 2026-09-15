/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
#define PA_REFCNT_DECLARE   int _ref
#define PA_REFCNT_INIT(p)   ((p)->_ref = 1)
#define PA_REFCNT_INC(p)    ((p)->_ref++)
#define PA_REFCNT_DEC(p)    (--((p)->_ref))
#define PA_REFCNT_VALUE(p)  ((p)->_ref)
