/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* A stand-in for the Android audio HAL.
 *
 * droid-pcm.c is the half of the plugin that touches hardware: it opens a HAL
 * stream, writes into it from its own thread, and drives the graph off a
 * timer. None of that can be exercised on a desk - and most of what can go
 * wrong in it has nothing to do with the hardware. The ring buffer, the
 * give-up-after-three-failures rule, the latency arithmetic, the drain at the
 * end of playback: all of those are decisions, and all of them have been wrong
 * at some point.
 *
 * So the HAL is replaced by something that counts bytes and can be told to
 * fail. What that proves is that the code around the HAL behaves; it proves
 * nothing about the HAL itself, and the README says so.
 */
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>
#include "pulsecore/core.h"
#include "pulsecore/refcnt.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"
#include <hardware/audio.h>
#include "droid/droid-util.h"

#include "hal-stub.h"

struct hal_stub hal_stub;

/* --- what the stub was asked to do ---------------------------------------- */

void hal_stub_reset(void)
{
	memset(&hal_stub, 0, sizeof(hal_stub));
	hal_stub.write_result = HAL_STUB_PASS;
	hal_stub.read_result = HAL_STUB_PASS;
	hal_stub.latency_ms = 42;
	hal_stub.buffer_size = 4096;
	hal_stub.open_output_works = true;
	hal_stub.open_input_works = true;
	hal_stub.module_works = true;
	hal_stub.is_primary = true;
	hal_stub.set_volume_result = 0;
	hal_stub.reconfigure_works = true;
	hal_stub.set_mode_works = true;
	hal_stub.set_input_device_works = true;
	hal_stub.rate = 48000;
	hal_stub.channels = 2;
}

/* --- the stream ----------------------------------------------------------- */

static ssize_t stub_write(struct audio_stream_out *stream, const void *buffer,
		size_t bytes)
{
	(void) stream; (void) buffer;
	hal_stub.writes++;
	hal_stub.bytes_written += bytes;
	if (hal_stub.write_result != HAL_STUB_PASS)
		return hal_stub.write_result;
	return (ssize_t) bytes;
}

static ssize_t stub_read(struct audio_stream_in *stream, void *buffer,
		size_t bytes)
{
	(void) stream;
	hal_stub.reads++;
	if (hal_stub.read_result != HAL_STUB_PASS)
		return hal_stub.read_result;
	/* Something other than silence, so a test can tell a real read from a
	 * buffer nobody filled. */
	memset(buffer, 0x21, bytes);
	hal_stub.bytes_read += bytes;
	return (ssize_t) bytes;
}

static int stub_set_volume(struct audio_stream_out *stream, float left,
		float right)
{
	(void) stream;
	hal_stub.volume_left = left;
	hal_stub.volume_right = right;
	hal_stub.set_volume_calls++;
	return hal_stub.set_volume_result;
}

/* Enough of the HAL structures for the pointers droid-pcm.c follows. */
static struct audio_stream_out stub_out;
static struct audio_stream_in stub_in;
static pa_droid_output_stream out_stream;
static pa_droid_input_stream in_stream;
static pa_droid_stream the_stream;
static pa_droid_hw_module the_module;
static audio_hw_device_t the_device;
static pa_sample_spec the_spec;

static int stub_set_parameters(struct audio_hw_device *dev, const char *kv)
{
	(void) dev;
	snprintf(hal_stub.last_parameters, sizeof(hal_stub.last_parameters), "%s", kv);
	hal_stub.parameter_calls++;
	return hal_stub.set_parameters_result;
}

static int stub_set_mode(struct audio_hw_device *dev, audio_mode_t mode)
{
	(void) dev;
	hal_stub.mode = mode;
	hal_stub.mode_calls++;
	return hal_stub.set_mode_result;
}

static int stub_set_voice_volume(struct audio_hw_device *dev, float volume)
{
	(void) dev;
	hal_stub.voice_volume = volume;
	hal_stub.voice_volume_calls++;
	return hal_stub.set_voice_volume_result;
}

pa_droid_hw_module *pa_droid_hw_module_get2(pa_core *core, pa_modargs *ma,
		const char *module_id)
{
	(void) core; (void) ma; (void) module_id;
	hal_stub.module_opens++;
	if (!hal_stub.module_works)
		return NULL;
	the_device.set_parameters = stub_set_parameters;
	the_device.set_mode = stub_set_mode;
	the_device.set_voice_volume = hal_stub.no_voice_volume ? NULL
		: stub_set_voice_volume;
	the_module.device = &the_device;
	the_module.enabled_module = hal_stub.enabled_module;
	the_module.options.enabled[DM_OPTION_REALCALL] = hal_stub.realcall;
	PA_REFCNT_INIT(&the_module);
	return &the_module;
}

pa_droid_hw_module *pa_droid_hw_module_ref(pa_droid_hw_module *hw)
{
	hal_stub.module_refs++;
	return hw;
}

void pa_droid_hw_module_unref(pa_droid_hw_module *hw)
{
	(void) hw;
	hal_stub.module_unrefs++;
}

void pa_droid_hw_module_lock(pa_droid_hw_module *hw) { (void) hw; hal_stub.locks++; }
void pa_droid_hw_module_unlock(pa_droid_hw_module *hw) { (void) hw; hal_stub.locks--; }

pa_droid_stream *pa_droid_open_output_stream(pa_droid_hw_module *module,
		const pa_sample_spec *spec, const pa_channel_map *map,
		dm_config_port *mix_port, dm_config_port *device_port)
{
	(void) module; (void) map; (void) mix_port; (void) device_port;
	hal_stub.output_opens++;
	if (!hal_stub.open_output_works)
		return NULL;
	stub_out.write = stub_write;
	stub_out.set_volume = hal_stub.no_set_volume ? NULL : stub_set_volume;
	out_stream.stream = &stub_out;
	the_stream.output = &out_stream;
	the_stream.input = NULL;
	the_stream.module = &the_module;
	the_spec = *spec;
	the_spec.rate = hal_stub.rate;
	the_spec.channels = hal_stub.channels;
	return &the_stream;
}

pa_droid_stream *pa_droid_open_input_stream(pa_droid_hw_module *module,
		const pa_sample_spec *spec, const pa_channel_map *map,
		const char *mix_port_name)
{
	(void) module; (void) map; (void) mix_port_name;
	hal_stub.input_opens++;
	if (!hal_stub.open_input_works)
		return NULL;
	stub_in.read = stub_read;
	in_stream.stream = &stub_in;
	the_stream.input = &in_stream;
	the_stream.output = NULL;
	the_stream.module = &the_module;
	the_spec = *spec;
	the_spec.rate = hal_stub.rate;
	the_spec.channels = hal_stub.channels;
	return &the_stream;
}

void pa_droid_stream_unref(pa_droid_stream *s) { (void) s; hal_stub.stream_unrefs++; }

const pa_sample_spec *pa_droid_stream_sample_spec(pa_droid_stream *s)
{
	(void) s;
	return &the_spec;
}

size_t pa_droid_stream_buffer_size(pa_droid_stream *s)
{
	(void) s;
	return hal_stub.buffer_size;
}

pa_usec_t pa_droid_stream_get_latency(pa_droid_stream *s)
{
	(void) s;
	return (pa_usec_t) hal_stub.latency_ms * 1000;
}

bool pa_droid_stream_is_primary(pa_droid_stream *s)
{
	(void) s;
	return hal_stub.is_primary;
}

int pa_droid_stream_set_route(pa_droid_stream *s, dm_config_port *port)
{
	(void) s;
	hal_stub.route_calls++;
	snprintf(hal_stub.last_route, sizeof(hal_stub.last_route), "%s",
			port && port->name ? port->name : "(none)");
	return hal_stub.set_route_result;
}

bool pa_droid_stream_reconfigure_input(pa_droid_stream *s,
		const pa_sample_spec *spec, const pa_channel_map *map,
		const pa_proplist *proplist)
{
	const char *source;
	(void) s; (void) spec; (void) map;
	hal_stub.reconfigure_calls++;
	source = proplist ? pa_proplist_gets(proplist, EXT_PROP_AUDIO_SOURCE) : NULL;
	snprintf(hal_stub.last_audio_source, sizeof(hal_stub.last_audio_source),
			"%s", source ? source : "");
	return hal_stub.reconfigure_works;
}

bool pa_droid_hw_set_mode(pa_droid_hw_module *hw, audio_mode_t mode)
{
	(void) hw;
	hal_stub.mode = mode;
	hal_stub.mode_calls++;
	return hal_stub.set_mode_works;
}

bool pa_droid_hw_set_input_device(pa_droid_stream *s, dm_config_port *port)
{
	(void) s;
	hal_stub.input_device_calls++;
	snprintf(hal_stub.last_input_device, sizeof(hal_stub.last_input_device),
			"%s", port && port->name ? port->name : "(none)");
	return hal_stub.set_input_device_works;
}

int pa_droid_set_parameters(pa_droid_hw_module *hw, const char *parameters)
{
	(void) hw;
	snprintf(hal_stub.last_parameters, sizeof(hal_stub.last_parameters), "%s",
			parameters ? parameters : "");
	hal_stub.parameter_calls++;
	return hal_stub.set_parameters_result;
}
