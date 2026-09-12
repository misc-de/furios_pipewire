/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* What the HAL stand-in was asked to do, and what it should answer. */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/* A write or read result of HAL_STUB_PASS means "behave": take everything,
 * deliver everything. Anything else - including a negative errno - is what the
 * call returns, which is the point of the whole stub. */
#define HAL_STUB_PASS ((ssize_t) INT32_MIN)

struct hal_stub {
	/* what happened */
	unsigned module_opens, module_refs, module_unrefs, locks;
	unsigned output_opens, input_opens, stream_unrefs;
	unsigned writes, reads;
	size_t bytes_written, bytes_read;
	unsigned route_calls, mode_calls, parameter_calls;
	unsigned voice_volume_calls, set_volume_calls, reconfigure_calls;
	unsigned input_device_calls;
	float volume_left, volume_right, voice_volume;
	int mode;
	char last_route[64], last_parameters[128], last_audio_source[64];
	char last_input_device[64];

	/* what it should answer */
	bool module_works, open_output_works, open_input_works;
	/* Let exactly this many opens fail and then work again. A bool cannot
	 * express "the move failed, the way back must not" - and that is the
	 * case worth testing, because it is the one that would otherwise leave a
	 * phone without sound. */
	unsigned output_opens_failing;
	bool reconfigure_works, set_mode_works, is_primary;
	bool set_input_device_works;
	bool no_set_volume, no_voice_volume;
	ssize_t write_result, read_result;   /* or HAL_STUB_PASS */
	int set_route_result, set_parameters_result, set_mode_result;
	int set_voice_volume_result, set_volume_result;
	unsigned latency_ms;
	size_t buffer_size;
	uint32_t rate, channels;
	bool realcall;             /* DM_OPTION_REALCALL on the module */
	void *enabled_module;      /* dm_config_module the HAL hands out */
};

extern struct hal_stub hal_stub;
void hal_stub_reset(void);
