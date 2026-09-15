/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Re-declares the PulseAudio interface the ported code expects, so it
 * compiles without libpulse. The names and signatures are dictated by
 * PulseAudio (LGPL-2.1+); the implementations behind them are ours.
 */
#pragma once
#include "pulsecore/core.h"
typedef struct pa_module pa_module;
