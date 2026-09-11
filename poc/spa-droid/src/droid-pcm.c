/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* SPA node: playback and capture through the Android audio HAL.
 *
 * The HAL write blocks until the data has been taken. It must therefore not
 * run on the graph's data thread, hence this layout:
 *
 *   process()        -> writes into a ring buffer      (graph thread)
 *   writer_thread()  -> reads from it, calls pa_droid_stream_write (own thread)
 *
 * Capture is the same thing backwards:
 *
 *   reader_thread()  -> pa_droid_stream_read, puts data into the ring buffer
 *   process()        -> takes it out, hands the buffer to the graph
 *
 * Both directions share this code; which one it is depends on the factory
 * used (api.droid.pcm or api.droid.pcm.source).
 */

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/system.h>
#include <spa/utils/hook.h>
#include <spa/utils/keys.h>
#include <spa/utils/names.h>
#include <spa/utils/result.h>
#include <spa/utils/ringbuffer.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/node/io.h>
#include <spa/node/utils.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/param/latency-utils.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>

#include <hardware/audio.h>
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>
#include "pulsecore/core.h"
#include "pulsecore/refcnt.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"
#include "droid/droid-util.h"
#include "droid/droid-config.h"
#include "droid/conversion.h"

#define NAME "droid-pcm"

/* The device (droid-device.c) and the node are separate SPA objects that live
 * in the same process. So that a route change on the device reaches the
 * running HAL stream, nodes register themselves here. Deliberately tiny:
 * there is never more than a handful of nodes. */
#define MAX_REG 8
static struct {
	pthread_mutex_t lock;
	struct { const char *mix_port; struct impl *node; } e[MAX_REG];
} registry = { .lock = PTHREAD_MUTEX_INITIALIZER };

struct impl;
static void registry_add(struct impl *this);
static void registry_remove(struct impl *this);

#define MAX_PORTS       1
#define RING_SIZE       (1u << 18)   /* 256 kB, power of two as spa_ringbuffer needs */
#define DEFAULT_RATE    48000
#define DEFAULT_CHANNELS 2

struct port {
	uint32_t id;
	bool have_format;
	struct spa_audio_info current_format;

	struct spa_io_buffers *io;

	struct spa_port_info info;
	struct spa_param_info params[5];

	struct buffer {
		struct spa_buffer *outbuf;
		bool queued;
	} buffers[32];
	uint32_t n_buffers;
};

struct impl {
	struct spa_handle handle;
	struct spa_node node;

	struct spa_log *log;
	struct spa_hook_list hooks;
	struct spa_callbacks callbacks;

	struct spa_node_info info;
	struct spa_param_info params[2];

	/* Direction: playback has an input port, capture an output port. */
	bool capture;
	enum spa_direction dir;

	struct port port;

	/* HAL */
	dm_config_device *config;
	dm_config_module *module;
	pa_droid_hw_module *hw;
	pa_droid_stream *stream;
	char mix_port_name[64];
	audio_devices_t input_device;
	char input_port_name[64];   /* empty = look the port up by device type */
	char audio_source[32];      /* Android audio source, e.g. "mic" */
	char hw_options[192];       /* options for the HAL module, see impl_init */
	char config_file[192];
	uint32_t pref_rate;         /* preferred values from the node properties */
	uint32_t pref_channels;
	char wanted_port[64];       /* route requested by the device */
	char wanted_route[64];      /* ... under the name the device uses for it */
	bool mode_holds_hal;        /* HAL only kept open for call mode */
	bool in_call;               /* mode is AUDIO_MODE_IN_CALL */
	uint64_t hal_latency_ns;    /* remembered at open time, see latency_ns() */
	bool hal_failed;            /* HAL persistently refuses to take data */

	/* handover to the writer thread */
	struct spa_ringbuffer ring;
	uint8_t *ring_data;
	pthread_t writer;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool running;
	bool started;
	bool drain;

	/* clock: a hardware sink drives the graph itself */
	struct spa_loop *data_loop;
	struct spa_system *data_system;
	struct spa_source timer_source;
	bool timer_added;
	struct spa_io_clock *clock;
	struct spa_io_position *position;
	uint64_t next_time;
	uint64_t period_ns;
	uint32_t quantum;
	uint32_t rate;

	/* instrumentation of the data path */
	bool diag;               /* verbose diagnostics (SPA_DROID_DIAG=1) */
	uint64_t bytes_queued;   /* put into the ring by process() */
	uint64_t bytes_written;  /* handed to the HAL by the writer thread */
	uint32_t n_process;
	uint32_t n_write;
	uint32_t n_write_err;    /* rejected HAL writes or reads */
	uint32_t n_overrun;      /* blocks dropped because the ring was full */
	uint32_t n_underrun;     /* capture: blocks padded with silence */
};

/* Diagnostics: info by default (invisible below PipeWire's log level), raised
 * to warn with SPA_DROID_DIAG=1 so they show without PIPEWIRE_DEBUG. */
#define DIAG(this, fmt, ...)						\
	do {								\
		if ((this)->diag)					\
			spa_log_warn((this)->log, NAME " [diag] " fmt, ##__VA_ARGS__); \
		else							\
			spa_log_info((this)->log, NAME " [diag] " fmt, ##__VA_ARGS__); \
	} while (0)

static int apply_route(struct impl *this, const char *route);
static int apply_mode(struct impl *this, const char *mode);
struct port;
static void emit_port_info(struct impl *this, struct port *port, bool full);
static void latency_changed(struct impl *this);
static int apply_voice_volume(struct impl *this, const char *value);
/* Bluetooth is commented out of this device's audio_policy XML, so the port
 * for it does not exist in the parsed configuration and has to be built.
 * That lookup lives further down, next to the route handling. */
static dm_config_port *port_by_route_name(struct impl *this, const char *route);
static void bt_sco_announce(struct impl *this, const dm_config_port *dev);

/* ------------------------------------------------------------------ HAL */

static int hal_open_input(struct impl *this, const pa_sample_spec *spec,
		const pa_channel_map *map)
{
	const pa_sample_spec *got;
	dm_config_port *mix, *dev;

	/* Same as on the output side: the HAL wants to know about Bluetooth
	 * before the stream exists, not after. */
	if (this->wanted_route[0])
		bt_sco_announce(this, port_by_route_name(this, this->wanted_route));

	/* Unlike the output side, the input takes the mix port by NAME - so the
	 * pointer-identity trap does not apply here. */
	this->stream = pa_droid_open_input_stream(this->hw, spec, map, this->mix_port_name);
	if (!this->stream) {
		spa_log_error(this->log, NAME " input stream \"%s\" failed",
				this->mix_port_name);
		return -EIO;
	}

	/* Merely opening gives the HAL AUDIO_SOURCE_DEFAULT. Android HALs tie
	 * their microphone processing (gain, noise suppression, echo
	 * cancellation) to the audio source, though. reconfigure_input sets it
	 * and reopens the stream on its own. */
	if (this->audio_source[0]) {
		pa_proplist *pl = pa_proplist_new();
		pa_proplist_sets(pl, EXT_PROP_AUDIO_SOURCE, this->audio_source);
		if (!pa_droid_stream_reconfigure_input(this->stream, spec, map, pl))
			spa_log_warn(this->log, NAME " audio source \"%s\" could not be set",
					this->audio_source);
		else
			DIAG(this, "audio source: %s", this->audio_source);
		pa_proplist_free(pl);
	}

	/* The HAL may change rate and channel count while opening. Our port has
	 * already negotiated a format, though - a deviation would silently bend
	 * pitch and channel mapping. Better to fail honestly and report the
	 * actual values. */
	got = pa_droid_stream_sample_spec(this->stream);
	if (got->rate != spec->rate || got->channels != spec->channels ||
	    got->format != spec->format) {
		spa_log_error(this->log, NAME " HAL delivered a different format than negotiated: "
				"%u Hz/%u channels/format %d instead of %u Hz/%u channels/format %d",
				got->rate, got->channels, got->format,
				spec->rate, spec->channels, spec->format);
		pa_droid_stream_unref(this->stream);
		this->stream = NULL;
		return -EINVAL;
	}

	mix = dm_config_find_mix_port(this->hw->enabled_module, this->mix_port_name);
	if (this->wanted_route[0])
		dev = port_by_route_name(this, this->wanted_route);
	else if (this->wanted_port[0])
		dev = dm_config_find_port(this->hw->enabled_module, this->wanted_port);
	else if (this->input_port_name[0])
		dev = dm_config_find_port(this->hw->enabled_module, this->input_port_name);
	else
		dev = mix ? dm_config_find_device_port(mix, this->input_device) : NULL;
	if (!dev)
		spa_log_warn(this->log, NAME " input device %#x not found - "
				"HAL keeps its current routing", this->input_device);
	else if (!pa_droid_hw_set_input_device(this->stream, dev))
		spa_log_warn(this->log, NAME " routing to \"%s\" failed", dev->name);
	else
		DIAG(this, "input device set: %s", dev->name);

	/* For input streams pa_droid_stream_get_latency() returns 0 - upstream
	 * never computes it. So estimate it ourselves: one HAL period. */
	{
		size_t bufsz = pa_droid_stream_buffer_size(this->stream);
		uint64_t bytes_per_sec = (uint64_t) spec->rate * 2 * spec->channels;
		this->hal_latency_ns = (bufsz && bytes_per_sec)
			? (uint64_t) bufsz * SPA_NSEC_PER_SEC / bytes_per_sec : 0;
	}
	latency_changed(this);
	spa_log_info(this->log, NAME " capture stream open: %s, %u Hz, %u channels, buffer %zu B",
			this->mix_port_name, spec->rate, spec->channels,
			pa_droid_stream_buffer_size(this->stream));
	return 0;
}

/* One reference that is never returned; see the note in hal_open(). */
static pa_droid_hw_module *hw_module_keepalive;

/* The HAL has to know about Bluetooth before the stream is opened.
 *
 * pa_droid_stream_set_route() sends BT_SCO=on as a side effect, but it needs
 * an open stream - so on the first open after a route change nothing had told
 * the HAL yet, and it opened a Bluetooth stream that stayed silent: the right
 * PCM device (pcmC0D55p), the chip even fetching the data, and nothing in the
 * ear. Android sets the parameter on the device, before opening. So do we. */
static void bt_sco_announce(struct impl *this, const dm_config_port *dev)
{
	bool bt;

	if (!this->hw || !dev)
		return;

	bt = dev->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO ||
	     dev->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET ||
	     dev->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO_CARKIT ||
	     dev->type == AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;

	if (pa_droid_set_parameters(this->hw, bt ? "BT_SCO=on" : "BT_SCO=off") < 0)
		spa_log_warn(this->log, NAME " the HAL did not take BT_SCO=%s",
				bt ? "on" : "off");
	else
		DIAG(this, "BT_SCO=%s sent before opening", bt ? "on" : "off");
}

static int hal_open(struct impl *this)
{
	dm_config_port *mix, *dev;
	pa_sample_spec spec;
	pa_channel_map map;

	if (this->stream)
		return 0;

	{
		/* Through modargs, so the module's vendor options take effect. This
		 * one is off on this device and concerns the voice path:
		 *
		 *   speaker_before_voice=true routes briefly to the speaker before
		 *                             the mode change; some devices start the
		 *                             call wrong otherwise.
		 *
		 * FuriOS loads module-droid-card without it. Set droid.hw-options in
		 * the node configuration to something else if you disagree.
		 *
		 * Options only take effect on the FIRST open of the module - after
		 * that it lives in the process-wide registry. */
		char args[512];
		pa_modargs *ma;

		snprintf(args, sizeof(args), "config=%s %s",
				this->config_file, this->hw_options);
		ma = pa_modargs_new(args, NULL);
		this->hw = pa_droid_hw_module_get2(pa_compat_core(), ma, "primary");
		pa_modargs_free(ma);
	}
	if (!this->hw) {
		spa_log_error(this->log, NAME " could not open the HAL module");
		return -EIO;
	}
	/* Keep the module loaded for the life of the process.
	 *
	 * Letting the last reference go unloads the Android side through
	 * libhybris, and the next open faults inside the Android linker's own
	 * initialisation:
	 *
	 *   android_linker_init () -> android_dlopen () -> hw_get_module_by_class ()
	 *   -> droid_hw_module_open () -> hal_open ()            SIGSEGV
	 *
	 * That is one open and close per suspend/resume, so it was a matter of
	 * time; it took down the whole daemon when playback fell back from a
	 * Bluetooth headset to the phone. PulseAudio keeps the module for as long
	 * as it runs, and so do we now. The module is not the exclusive part -
	 * the stream is, and that is still opened and closed as before. */
	if (hw_module_keepalive == NULL)
		hw_module_keepalive = pa_droid_hw_module_ref(this->hw);
	DIAG(this, "HAL options: %s", this->hw_options[0] ? this->hw_options : "(none)");

	if (this->capture) {
		int res;
		spec.format = PA_SAMPLE_S16LE;
		spec.rate = this->port.have_format
			? this->port.current_format.info.raw.rate : DEFAULT_RATE;
		spec.channels = this->port.have_format
			? this->port.current_format.info.raw.channels : DEFAULT_CHANNELS;
		if (spec.channels == 1)
			pa_channel_map_init_mono(&map);
		else
			pa_channel_map_init_stereo(&map);

		if ((res = hal_open_input(this, &spec, &map)) < 0) {
			pa_droid_hw_module_unref(this->hw);
			this->hw = NULL;
			return res;
		}
		this->bytes_queued = this->bytes_written = 0;
		this->n_process = this->n_write = 0;
		this->n_write_err = this->n_overrun = this->n_underrun = 0;
		this->hal_failed = false;
		return 0;
	}

	/* IMPORTANT: pa_droid_hw_module_get duplicates the configuration
	 * (dm_config_dup). pa_droid_open_output_stream compares ports by POINTER
	 * IDENTITY against hw->enabled_module. Ports from our own copy are
	 * therefore always rejected - they must come from the HAL module. */
	mix = dm_config_find_mix_port(this->hw->enabled_module, this->mix_port_name);
	/* Not dm_config_find_port() alone: the Bluetooth ports are missing from
	 * this device's audio_policy XML, so a name lookup returns nothing and
	 * the fallback below would quietly play to the speaker instead - which is
	 * exactly what it did, while every log line said "BT SCO". */
	dev = this->wanted_route[0] ? port_by_route_name(this, this->wanted_route) : NULL;
	if (!dev && this->wanted_port[0])
		dev = dm_config_find_port(this->hw->enabled_module, this->wanted_port);
	if (!dev) {
		if (this->wanted_route[0])
			spa_log_warn(this->log, NAME " route \"%s\" is not available - "
					"falling back to the default output",
					this->wanted_route);
		dev = dm_config_default_output_device(this->hw->enabled_module);
	}
	if (!mix || !dev) {
		spa_log_error(this->log, NAME " mix port \"%s\" or default output missing",
				this->mix_port_name);
		pa_droid_hw_module_unref(this->hw);
		this->hw = NULL;
		return -ENOENT;
	}

	spec.format = PA_SAMPLE_S16LE;
	spec.rate = this->port.have_format
		? this->port.current_format.info.raw.rate : this->pref_rate;
	spec.channels = this->port.have_format
		? this->port.current_format.info.raw.channels : this->pref_channels;
	if (spec.channels == 1)
		pa_channel_map_init_mono(&map);
	else
		pa_channel_map_init_stereo(&map);

	bt_sco_announce(this, dev);

	this->stream = pa_droid_open_output_stream(this->hw, &spec, &map, mix, dev);
	if (!this->stream) {
		spa_log_error(this->log, NAME " output stream \"%s\" -> \"%s\" failed",
				mix->name, dev->name);
		pa_droid_hw_module_unref(this->hw);
		this->hw = NULL;
		return -EIO;
	}

	this->hal_latency_ns = (uint64_t) pa_droid_stream_get_latency(this->stream) * 1000;
	latency_changed(this);
	/* The HAL may override the requested values - for voip_rx it always
	 * does. Miss that and audio plays at the wrong speed. So check and say
	 * so. */
	{
		const pa_sample_spec *got = pa_droid_stream_sample_spec(this->stream);
		if (got && (got->rate != spec.rate || got->channels != spec.channels))
			spa_log_warn(this->log, NAME " HAL took %u Hz/%u channels instead of "
					"%u Hz/%u - audio will sound out of tune",
					got->rate, got->channels, spec.rate, spec.channels);
	}
	spa_log_info(this->log, NAME " stream open: %s -> %s, %u Hz, %u channels, buffer %zu B",
			mix->name, dev->name, spec.rate, spec.channels,
			pa_droid_stream_buffer_size(this->stream));

	/* Without these two steps the stream opens but stays silent.
	 * PulseAudio's droid-sink does exactly the same
	 * (do_routing/update_volumes). */
	/* ONLY on the primary stream: pa_droid_stream_set_route() checks this
	 * with an assertion and aborts the whole process if you try it on another
	 * mix port (voip_rx, for instance). Routing applies to all open streams
	 * anyway - the primary one sets it. */
	if (!pa_droid_stream_is_primary(this->stream))
		DIAG(this, "not the primary stream - the primary one does the routing");
	else if (pa_droid_stream_set_route(this->stream, dev) < 0)
		spa_log_warn(this->log, NAME " Routing auf \"%s\" failed", dev->name);
	else
		DIAG(this, "routing set: %s", dev->name);

	/* Full scale, and it stays there: the level is applied in the graph.
	 * This HAL accepts set_volume on the primary output and returns success,
	 * but the measured level does not move (RMS 5796 at 100 %, 5734 at 20 %)
	 * - on Android that gain sits in AudioFlinger, above the HAL. */
	pa_droid_hw_module_lock(this->hw);
	if (this->stream->output->stream->set_volume) {
		int r = this->stream->output->stream->set_volume(
				this->stream->output->stream, 1.0f, 1.0f);
		DIAG(this, "HAL volume set to 1.0 (ret %d)", r);
	} else {
		spa_log_warn(this->log, NAME " HAL offers no set_volume - level stays at the HAL default");
	}
	pa_droid_hw_module_unlock(this->hw);

	this->bytes_queued = this->bytes_written = 0;
	this->n_process = this->n_write = 0;
	this->n_write_err = this->n_overrun = this->n_underrun = 0;
	this->hal_failed = false;
	return 0;
}

static void hal_close(struct impl *this)
{
	bool had_stream = this->stream != NULL;

	if (this->stream) {
		pa_droid_stream_unref(this->stream);
		this->stream = NULL;
	}
	if (this->hw) {
		pa_droid_hw_module_unref(this->hw);
		this->hw = NULL;
	}
	/* No stream means no delay any more - otherwise the old value would
	 * stand and the graph would account for a latency that no longer
	 * exists. */
	this->hal_latency_ns = 0;
	if (had_stream)
		latency_changed(this);
}

/* How far does audio lag behind the picture?
 *
 * PipeWire cannot guess: it knows neither the HAL's buffers nor our ring
 * buffer in between. Without a report it assumes zero - and then audio runs
 * ahead of video. What we report is the sum of both. */
static uint64_t latency_ns(struct impl *this)
{
	uint32_t stride, idx;
	uint64_t ns;
	int32_t avail;

	/* The HAL's share is queried once at open time and remembered:
	 * pa_droid_stream_get_latency() calls into the HAL, and this function
	 * runs on the graph's data path. A blocking call there would be a
	 * dropout. */
	ns = this->hal_latency_ns;

	stride = 2 * (this->port.have_format
			? this->port.current_format.info.raw.channels : DEFAULT_CHANNELS);
	avail = spa_ringbuffer_get_read_index(&this->ring, &idx);
	if (avail > 0 && this->rate && stride)
		ns += (uint64_t) avail * SPA_NSEC_PER_SEC / ((uint64_t) this->rate * stride);

	return ns;
}

/* The latency changes when the HAL stream opens - then it must be reported
 * again. Flip the SERIAL bit, or nobody notices. */
static void latency_changed(struct impl *this)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(this->port.params); i++)
		if (this->port.params[i].id == SPA_PARAM_Latency)
			this->port.params[i].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(this, &this->port, false);
}

/* --------------------------------------------------------- clock */

static int set_timeout(struct impl *this, uint64_t time)
{
	struct itimerspec ts;

	ts.it_value.tv_sec  = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	return spa_system_timerfd_settime(this->data_system, this->timer_source.fd,
			SPA_FD_TIMER_ABSTIME, &ts, NULL);
}

static void timer_stop(struct impl *this)
{
	struct itimerspec ts = { { 0, 0 }, { 0, 0 } };
	if (this->data_system && this->timer_source.fd >= 0)
		spa_system_timerfd_settime(this->data_system, this->timer_source.fd, 0, &ts, NULL);
}

/* One graph cycle per tick - that is what makes process() run at all. */
static void on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t expirations, nsec;

	if (spa_system_timerfd_read(this->data_system, this->timer_source.fd, &expirations) < 0)
		return;

	nsec = this->next_time;

	/* The quantum size belongs to the graph, not to us. It need not match
	 * the HAL buffer size - the ring buffer decouples the two. Without this
	 * adjustment the clock would keep running at its start value while the
	 * graph already processes a different size. */
	if (this->position && this->position->clock.target_duration &&
	    this->position->clock.target_duration != this->quantum && this->rate) {
		this->quantum = this->position->clock.target_duration;
		this->period_ns = (uint64_t) this->quantum * SPA_NSEC_PER_SEC / this->rate;
		DIAG(this, "quantum changed by the graph: %u frames every %llu us",
				this->quantum, (unsigned long long) (this->period_ns / 1000));
	}

	if (this->clock) {
		this->clock->nsec = nsec;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = this->quantum;
		/* In samples, as PipeWire expects it. */
		this->clock->delay = this->rate
			? (int64_t) (latency_ns(this) * this->rate / SPA_NSEC_PER_SEC) : 0;
		this->clock->rate_diff = 1.0;
		this->clock->next_nsec = nsec + this->period_ns;
	}

	spa_node_call_ready(&this->callbacks, SPA_STATUS_HAVE_DATA);

	this->next_time += this->period_ns;
	set_timeout(this, this->next_time);
}

static int timer_start(struct impl *this)
{
	struct timespec now;
	uint32_t rate = this->port.have_format
		? this->port.current_format.info.raw.rate : DEFAULT_RATE;
	uint32_t channels = this->port.have_format
		? this->port.current_format.info.raw.channels : DEFAULT_CHANNELS;
	size_t bufsz = pa_droid_stream_buffer_size(this->stream);
	int res;

	/* Period from the HAL buffer size: buffer_size / (2 bytes * channels) frames */
	this->rate = rate;
	this->quantum = bufsz ? (uint32_t) (bufsz / (2 * channels)) : 1024;
	if (this->quantum == 0)
		this->quantum = 1024;
	if (this->position && this->position->clock.target_duration)
		this->quantum = this->position->clock.target_duration;
	this->period_ns = (uint64_t) this->quantum * SPA_NSEC_PER_SEC / rate;

	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now) + this->period_ns;
	/* A timer that cannot be armed means no graph cycles at all - the node
	 * would sit there looking healthy and play nothing. */
	if ((res = set_timeout(this, this->next_time)) < 0)
		return res;

	DIAG(this, "clock running: %u frames every %llu us",
			this->quantum, (unsigned long long) (this->period_ns / 1000));
	return 0;
}

/* --------------------------------------------------- writer thread */

static void *writer_thread(void *arg)
{
	struct impl *this = arg;
	size_t chunk = pa_droid_stream_buffer_size(this->stream);
	uint8_t *buf;
	unsigned consecutive_errors = 0;

	if (chunk == 0 || chunk > RING_SIZE / 2)
		chunk = 4096;
	buf = malloc(chunk);
	if (!buf)
		return NULL;

	while (true) {
		uint32_t idx;
		int32_t avail;
		size_t take;

		pthread_mutex_lock(&this->lock);
		while (this->running &&
		       (avail = spa_ringbuffer_get_read_index(&this->ring, &idx)) < (int32_t) chunk)
			pthread_cond_wait(&this->cond, &this->lock);
		if (!this->running) {
			/* Do not leave the remainder behind when stopping: whatever is
			 * still in the ring gets padded with silence to a full HAL
			 * buffer and written out. Otherwise the last ~21 ms are lost. */
			avail = spa_ringbuffer_get_read_index(&this->ring, &idx);
			if (!this->drain || avail <= 0) {
				pthread_mutex_unlock(&this->lock);
				break;
			}
		}
		pthread_mutex_unlock(&this->lock);

		take = SPA_MIN((size_t) avail, chunk);
		spa_ringbuffer_read_data(&this->ring, this->ring_data, RING_SIZE,
				idx & (RING_SIZE - 1), buf, take);
		spa_ringbuffer_read_update(&this->ring, idx + take);
		if (take < chunk) {
			memset(buf + take, 0, chunk - take);
			DIAG(this, "remainder written out: %zu B data + %zu B silence",
					take, chunk - take);
		}

		{
			ssize_t w = pa_droid_stream_write(this->stream, buf, chunk);
			if (w < 0) {
				/* Do not log per quantum - with a persistent failure that
				 * would be a log storm. First error plus the summary is
				 * enough. */
				if (this->n_write_err++ == 0)
					spa_log_warn(this->log, NAME " HAL write failed: %zd "
							"(further ones are only counted)", w);
				/* Three failures in a row mean the stream is gone. Writing
				 * on achieves nothing and only burns power. Give up, but
				 * cleanly - the next start reopens everything in hal_open().
				 * The node gets there on its own as soon as WirePlumber
				 * suspends it while idle. */
				if (++consecutive_errors >= 3 && !this->hal_failed) {
					this->hal_failed = true;
					spa_log_error(this->log, NAME " HAL takes nothing any more - "
							"playback stopped. The next start will "
							"reopen it.");
				}
			} else {
				consecutive_errors = 0;
				if (this->n_write == 0)
					DIAG(this, "first HAL write ok: %zd of %zu B", w, chunk);
				this->bytes_written += (uint64_t) w;
				this->n_write++;
			}
		}
	}

	free(buf);
	return NULL;
}

/* Capture: pa_droid_stream_read blocks until the next HAL period and thereby
 * sets the pace. Hence a thread of its own as well. */
static void *reader_thread(void *arg)
{
	struct impl *this = arg;
	size_t chunk = pa_droid_stream_buffer_size(this->stream);
	uint8_t *buf;
	unsigned consecutive_errors = 0;

	if (chunk == 0 || chunk > RING_SIZE / 2)
		chunk = 4096;
	buf = malloc(chunk);
	if (!buf)
		return NULL;

	while (true) {
		ssize_t r;
		uint32_t idx;
		int32_t filled;
		bool run;

		pthread_mutex_lock(&this->lock);
		run = this->running;
		pthread_mutex_unlock(&this->lock);
		if (!run)
			break;

		r = pa_droid_stream_read(this->stream, buf, chunk);
		if (r <= 0) {
			if (this->n_write_err++ == 0)
				spa_log_warn(this->log, NAME " HAL read failed: %zd "
						"(further ones are only counted)", r);
			if (++consecutive_errors >= 3 && !this->hal_failed) {
				this->hal_failed = true;
				spa_log_error(this->log, NAME " HAL delivers nothing any more - "
						"capture stopped. The next start will "
						"reopen it.");
			}
			/* Do not spin if the HAL fails immediately and permanently. */
			usleep((useconds_t) (this->period_ns / 1000));
			continue;
		}
		consecutive_errors = 0;

		if (this->n_write == 0)
			DIAG(this, "first HAL read ok: %zd of %zu B", r, chunk);
		this->bytes_written += (uint64_t) r;
		this->n_write++;

		filled = spa_ringbuffer_get_write_index(&this->ring, &idx);
		if (filled + r > (int32_t) RING_SIZE) {
			/* Nobody is picking the data up - drop the oldest rather than the
			 * newest, otherwise capture falls further and further behind. */
			if (this->n_overrun++ == 0)
				spa_log_warn(this->log, NAME " ring buffer full, capture drops "
						"oldest data (further ones are only counted)");
			spa_ringbuffer_read_update(&this->ring,
					idx + filled + (int32_t) r - (int32_t) RING_SIZE);
		}
		spa_ringbuffer_write_data(&this->ring, this->ring_data, RING_SIZE,
				idx & (RING_SIZE - 1), buf, (uint32_t) r);
		spa_ringbuffer_write_update(&this->ring, idx + (uint32_t) r);
	}

	free(buf);
	return NULL;
}

static int writer_start(struct impl *this)
{
	if (this->started)
		return 0;
	/* Empty the ring: an aborted run must not let the next one start with
	 * stale material. The graph is not running yet at this point. */
	spa_ringbuffer_init(&this->ring);
	this->running = true;
	this->drain = true;
	/* pthread_create RETURNS the error number and does not touch errno -
	 * reading errno here gave 0 on failure, which reads as success. */
	{
		int err = pthread_create(&this->writer, NULL,
				this->capture ? reader_thread : writer_thread, this);
		if (err != 0) {
			this->running = false;
			return -err;
		}
	}
	this->started = true;
	return 0;
}

/* drain=false discards the remainder immediately (emergency exit),
 * drain=true still writes it out, padded with silence. */
static void writer_stop(struct impl *this, bool drain)
{
	if (!this->started)
		return;
	pthread_mutex_lock(&this->lock);
	this->drain = drain;
	this->running = false;
	pthread_cond_broadcast(&this->cond);
	pthread_mutex_unlock(&this->lock);
	pthread_join(this->writer, NULL);
	this->started = false;
	DIAG(this, "summary: process() %ux / %llu B, HAL %s %ux / %llu B",
			this->n_process, (unsigned long long) this->bytes_queued,
			this->capture ? "Read" : "Write",
			this->n_write, (unsigned long long) this->bytes_written);
	if (this->n_underrun)
		DIAG(this, "%u blocks padded with silence (ring was empty)", this->n_underrun);
	if (this->n_write_err || this->n_overrun)
		spa_log_warn(this->log, NAME " trouble during the run: %u rejected HAL %s, "
				"%u dropped blocks (ring full)",
				this->n_write_err, this->capture ? "Reads" : "Writes",
				this->n_overrun);
}

/* ------------------------------------------------------------- Node */

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	/* props MUST be set: libpipewire-module-adapter passes info->props on to
	 * pw_properties_update unchecked - NULL segfaults there. */
	struct spa_dict_item items[3];
	uint32_t n = 0;

	/* "droid-hal" is the identifier PulseAudio's droid module uses.
	 * callaudiod recognises an Android card by nothing else. */
	items[n++] = SPA_DICT_ITEM_INIT("device.api", "droid-hal");
	items[n++] = SPA_DICT_ITEM_INIT("media.class",
			this->capture ? "Audio/Source" : "Audio/Sink");
	items[n++] = SPA_DICT_ITEM_INIT("droid.mix-port", this->mix_port_name);
	this->info.props = &SPA_DICT_INIT(items, n);

	if (full)
		this->info.change_mask = SPA_NODE_CHANGE_MASK_FLAGS |
					 SPA_NODE_CHANGE_MASK_PROPS |
					 SPA_NODE_CHANGE_MASK_PARAMS;
	if (this->info.change_mask) {
		/* Emitting is synchronous - the stack dict lives long enough. */
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
	this->info.props = NULL;
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	struct spa_dict_item items[1];

	/* same trap as on the node: props must not be NULL */
	items[0] = SPA_DICT_ITEM_INIT("port.name", this->capture ? "capture" : "playback");
	port->info.props = &SPA_DICT_INIT(items, 1);

	if (full)
		port->info.change_mask = SPA_PORT_CHANGE_MASK_FLAGS |
					 SPA_PORT_CHANGE_MASK_PROPS |
					 SPA_PORT_CHANGE_MASK_PARAMS;
	if (port->info.change_mask) {
		spa_node_emit_port_info(&this->hooks,
				this->dir, port->id, &port->info);
		port->info.change_mask = old;
	}
	port->info.props = NULL;
}

static int impl_add_listener(void *object, struct spa_hook *listener,
		const struct spa_node_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_node_info(this, true);
	emit_port_info(this, &this->port, true);

	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_set_callbacks(void *object,
		const struct spa_node_callbacks *callbacks, void *data)
{
	struct impl *this = object;
	spa_return_val_if_fail(this != NULL, -EINVAL);
	this->callbacks = SPA_CALLBACKS_INIT(callbacks, data);
	return 0;
}

static int impl_send_command(void *object, const struct spa_command *command)
{
	struct impl *this = object;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (SPA_NODE_COMMAND_ID(command)) {
	case SPA_NODE_COMMAND_Start:
		if (!this->port.have_format) {
			spa_log_error(this->log, NAME " start without a negotiated format");
			return -EIO;
		}
		if ((res = hal_open(this)) < 0)
			return res;
		/* Roll partial state back: otherwise an open HAL stream or a running
		 * thread would be left behind while the node counts as failed. */
		if ((res = writer_start(this)) < 0) {
			spa_log_error(this->log, NAME " could not start the writer thread: %s",
					spa_strerror(res));
			hal_close(this);
			return res;
		}
		if ((res = timer_start(this)) < 0) {
			spa_log_error(this->log, NAME " could not start the clock: %s",
					spa_strerror(res));
			writer_stop(this, false);
			hal_close(this);
			return res;
		}
		DIAG(this, "start command received, writer running");
		break;
	case SPA_NODE_COMMAND_Pause:
		timer_stop(this);
		writer_stop(this, true);
		spa_log_info(this->log, NAME " stopped");
		break;
	case SPA_NODE_COMMAND_Suspend:
		/* Suspend releases the hardware - only then can PulseAudio get the
		 * PCM device back while the sink merely sits idle. During a call it
		 * stays open: there the voice path runs through modem and DSP without
		 * any PipeWire stream playing. */
		timer_stop(this);
		writer_stop(this, true);
		if (this->mode_holds_hal) {
			spa_log_info(this->log, NAME " stopped, HAL stays open for the call");
		} else {
			hal_close(this);
			spa_log_info(this->log, NAME " stopped, HAL released");
		}
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

/* Channel positions do NOT belong here: a channel range and fixed positions
 * are mutually exclusive within one format object, and with two fixed variants
 * the adapter promptly negotiated mono. The mapping comes from the node
 * property audio.position (FL,FR) that the device provides. */
static int port_enum_formats(struct impl *this, struct spa_pod_builder *b,
		uint32_t index, struct spa_pod **param)
{
	if (index > 0)
		return 0;

	/* The preferred value goes first in the choice - otherwise the adapter
	 * takes the default and ignores audio.rate from the node properties. That
	 * matters for the VoIP channel: there the HAL insists on 16 kHz. */
	*param = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
		SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
		SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(
						(int) this->pref_rate, 8000, 48000),
		SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(
						(int) this->pref_channels, 1, 2));
	return 1;
}

static int impl_set_io(void *object, uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_IO_Clock:
		this->clock = (size >= sizeof(struct spa_io_clock)) ? data : NULL;
		break;
	case SPA_IO_Position:
		this->position = (size >= sizeof(struct spa_io_position)) ? data : NULL;
		break;
	default:
		return -ENOENT;
	}
	return 0;
}

static int impl_port_enum_params(void *object, int seq,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, uint32_t start, uint32_t num,
		const struct spa_pod *filter)
{
	struct impl *this = object;
	struct port *port = &this->port;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[1024];
	struct spa_result_node_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == this->dir, -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumFormat:
		if ((res = port_enum_formats(this, &b, result.index,
						(struct spa_pod **) &result.param)) <= 0)
			return res;
		break;
	case SPA_PARAM_Format:
		if (!port->have_format)
			return -EIO;
		if (result.index > 0)
			return 0;
		result.param = spa_format_audio_raw_build(&b, id,
				&port->current_format.info.raw);
		break;
	case SPA_PARAM_Buffers:
	{
		/* The buffer must hold one GRAPH quantum, not one HAL period. On the
		 * output side both happen to be the same size (4096 B), on the input
		 * side they are not: the HAL delivers 3840 B while the graph wants
		 * 4096 B. Too small a buffer makes the adapter work with a partial
		 * quantum. */
		uint32_t stride = 2 * port->current_format.info.raw.channels;
		uint32_t q = this->quantum;
		size_t size;

		if (!port->have_format)
			return -EIO;
		if (q == 0 && this->position)
			q = this->position->clock.target_duration;
		if (q == 0)
			q = 1024;
		size = SPA_MAX((size_t) q * stride,
				this->stream ? pa_droid_stream_buffer_size(this->stream) : 4096);
		if (result.index > 0)
			return 0;
		result.param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamBuffers, id,
			SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(2, 2, 16),
			SPA_PARAM_BUFFERS_blocks,  SPA_POD_Int(1),
			SPA_PARAM_BUFFERS_size,    SPA_POD_Int((int) size),
			SPA_PARAM_BUFFERS_stride,  SPA_POD_Int((int) stride));
		break;
	}
	case SPA_PARAM_IO:
		if (result.index > 0)
			return 0;
		result.param = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_ParamIO, id,
			SPA_PARAM_IO_id,   SPA_POD_Id(SPA_IO_Buffers),
			SPA_PARAM_IO_size, SPA_POD_Int(sizeof(struct spa_io_buffers)));
		break;
	case SPA_PARAM_Latency:
	{
		struct spa_latency_info info;
		uint64_t ns;
		if (result.index > 0)
			return 0;
		ns = latency_ns(this);
		/* A sink reports the delay downstream, a source the delay upstream -
		 * hence the direction of our own port. */
		info = SPA_LATENCY_INFO(this->dir,
				.min_ns = (int64_t) ns,
				.max_ns = (int64_t) ns);
		result.param = spa_latency_build(&b, id, &info);
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, (struct spa_pod **) &result.param, result.param, filter) < 0)
		goto next;

	spa_node_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_NODE_PARAMS, &result);
	if (++count != num)
		goto next;

	return 0;
}

static int port_set_format(struct impl *this, struct port *port,
		uint32_t flags, const struct spa_pod *format)
{
	int res;

	if (format == NULL) {
		/* No format means the port is being torn down. The clock has to go
		 * with it, or it keeps ticking without a data path. */
		timer_stop(this);
		writer_stop(this, true);
		hal_close(this);
		port->have_format = false;
		return 0;
	}

	struct spa_audio_info info = { 0 };
	if ((res = spa_format_parse(format, &info.media_type, &info.media_subtype)) < 0)
		return res;
	if (info.media_type != SPA_MEDIA_TYPE_audio ||
	    info.media_subtype != SPA_MEDIA_SUBTYPE_raw)
		return -EINVAL;
	if (spa_format_audio_raw_parse(format, &info.info.raw) < 0)
		return -EINVAL;
	if (info.info.raw.format != SPA_AUDIO_FORMAT_S16_LE)
		return -EINVAL;

	port->current_format = info;
	port->have_format = true;

	port->info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_READWRITE);
	emit_port_info(this, port, false);
	return 0;
}

static int impl_port_set_param(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(direction == this->dir, -EINVAL);

	if (id == SPA_PARAM_Format)
		return port_set_format(this, &this->port, flags, param);
	return -ENOENT;
}

static int impl_port_use_buffers(void *object,
		enum spa_direction direction, uint32_t port_id, uint32_t flags,
		struct spa_buffer **buffers, uint32_t n_buffers)
{
	struct impl *this = object;
	struct port *port = &this->port;
	uint32_t i;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	for (i = 0; i < n_buffers && i < SPA_N_ELEMENTS(port->buffers); i++) {
		port->buffers[i].outbuf = buffers[i];
		port->buffers[i].queued = false;
	}
	port->n_buffers = i;
	return 0;
}

static int impl_port_set_io(void *object,
		enum spa_direction direction, uint32_t port_id,
		uint32_t id, void *data, size_t size)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (id == SPA_IO_Buffers)
		this->port.io = data;
	else
		return -ENOENT;
	return 0;
}

/* Capture: take a free buffer, fill it from the ring, hand it to the graph.
 * If the ring is empty (start-up, dropout) it is padded with silence - a short
 * buffer would be an error as far as the graph is concerned. */
static int process_capture(struct impl *this)
{
	struct port *port = &this->port;
	struct spa_io_buffers *io = port->io;
	struct spa_data *d;
	uint32_t idx, want, take, stride, id;
	int32_t avail;

	if (io == NULL)
		return -EIO;
	if (io->status == SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_HAVE_DATA;

	if (io->buffer_id < port->n_buffers) {
		port->buffers[io->buffer_id].queued = false;
		io->buffer_id = SPA_ID_INVALID;
	}

	for (id = 0; id < port->n_buffers; id++)
		if (!port->buffers[id].queued)
			break;
	if (id == port->n_buffers) {
		io->status = -EPIPE;
		return SPA_STATUS_HAVE_DATA;
	}

	d = &port->buffers[id].outbuf->datas[0];
	stride = 2 * port->current_format.info.raw.channels;
	want = SPA_MIN(d->maxsize, this->quantum * stride);

	avail = spa_ringbuffer_get_read_index(&this->ring, &idx);
	take = SPA_MIN((uint32_t) SPA_MAX(avail, 0), want);
	if (take > 0) {
		spa_ringbuffer_read_data(&this->ring, this->ring_data, RING_SIZE,
				idx & (RING_SIZE - 1), d->data, take);
		spa_ringbuffer_read_update(&this->ring, idx + take);
	}
	if (take < want) {
		memset(SPA_PTROFF(d->data, take, void), 0, want - take);
		this->n_underrun++;
	}

	d->chunk->offset = 0;
	d->chunk->size = want;
	d->chunk->stride = stride;

	if (this->n_process == 0)
		DIAG(this, "first process(): %u B delivered (%u B from the ring)", want, take);
	this->bytes_queued += take;
	this->n_process++;

	port->buffers[id].queued = true;
	io->buffer_id = id;
	io->status = SPA_STATUS_HAVE_DATA;
	return SPA_STATUS_HAVE_DATA;
}

static int impl_process(void *object)
{
	struct impl *this = object;
	struct port *port = &this->port;
	struct spa_io_buffers *io;
	struct spa_buffer *buf;
	struct spa_data *d;
	uint32_t idx, filled, offs, size;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (this->capture)
		return process_capture(this);

	if ((io = port->io) == NULL)
		return -EIO;
	if (io->status != SPA_STATUS_HAVE_DATA)
		return SPA_STATUS_OK;
	if (io->buffer_id >= port->n_buffers)
		return -EINVAL;

	buf = port->buffers[io->buffer_id].outbuf;
	d = &buf->datas[0];
	offs = SPA_MIN(d->chunk->offset, d->maxsize);
	size = SPA_MIN(d->chunk->size, d->maxsize - offs);

	/* If the HAL takes nothing any more, stop filling the ring - otherwise it
	 * overflows and we count losses for nothing. */
	if (this->hal_failed) {
		io->status = SPA_STATUS_NEED_DATA;
		return SPA_STATUS_NEED_DATA;
	}

	filled = spa_ringbuffer_get_write_index(&this->ring, &idx);
	if (filled + size > RING_SIZE) {
		if (this->n_overrun++ == 0)
			spa_log_warn(this->log, NAME " ring buffer full, %u bytes dropped "
					"(further ones are only counted)", size);
	} else {
		spa_ringbuffer_write_data(&this->ring, this->ring_data, RING_SIZE,
				idx & (RING_SIZE - 1),
				SPA_PTROFF(d->data, offs, void), size);
		spa_ringbuffer_write_update(&this->ring, idx + size);
		if (this->n_process == 0)
			DIAG(this, "first process(): %u B queued", size);
		this->bytes_queued += size;
		this->n_process++;

		pthread_mutex_lock(&this->lock);
		pthread_cond_signal(&this->cond);
		pthread_mutex_unlock(&this->lock);
	}

	io->status = SPA_STATUS_NEED_DATA;
	return SPA_STATUS_NEED_DATA;
}

/* The only way a route change from the device reaches this code: device and
 * node run in different processes (device in WirePlumber, node in the PipeWire
 * daemon). WirePlumber passes the route name across as an SPA_PROP_params
 * pair. */
static int impl_node_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *this = object;
	const struct spa_pod_prop *prop;
	const struct spa_pod_object *obj;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	if (id != SPA_PARAM_Props || param == NULL)
		return -ENOENT;

	obj = (const struct spa_pod_object *) param;
	SPA_POD_OBJECT_FOREACH(obj, prop) {
		struct spa_pod_parser prs;
		struct spa_pod_frame f;

		if (prop->key != SPA_PROP_params)
			continue;

		spa_pod_parser_pod(&prs, &prop->value);
		if (spa_pod_parser_push_struct(&prs, &f) < 0)
			continue;
		while (true) {
			const char *key, *val;
			if (spa_pod_parser_get_string(&prs, &key) < 0)
				break;
			if (spa_pod_parser_get_string(&prs, &val) < 0)
				break;
			if (spa_streq(key, "droid.route"))
				apply_route(this, val);
			else if (spa_streq(key, "droid.mode"))
				apply_mode(this, val);
			else if (spa_streq(key, "droid.voice-volume"))
				apply_voice_volume(this, val);
		}
		spa_pod_parser_pop(&prs, &f);
	}
	return 0;
}

static const struct spa_node_methods impl_node = {
	SPA_VERSION_NODE_METHODS,
	.add_listener = impl_add_listener,
	.set_param = impl_node_set_param,
	.set_callbacks = impl_set_callbacks,
	.set_io = impl_set_io,
	.send_command = impl_send_command,
	.port_enum_params = impl_port_enum_params,
	.port_set_param = impl_port_set_param,
	.port_use_buffers = impl_port_use_buffers,
	.port_set_io = impl_port_set_io,
	.process = impl_process,
};

/* --------------------------------------------------------- Handle */

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this = (struct impl *) handle;

	spa_return_val_if_fail(handle != NULL && interface != NULL, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_Node))
		*interface = &this->node;
	else
		return -ENOENT;
	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *) handle;

	registry_remove(this);
	timer_stop(this);
	if (this->timer_added) {
		spa_loop_remove_source(this->data_loop, &this->timer_source);
		this->timer_added = false;
	}
	if (this->timer_source.fd >= 0) {
		spa_system_close(this->data_system, this->timer_source.fd);
		this->timer_source.fd = -1;
	}
	writer_stop(this, false);
	hal_close(this);
	if (this->config) {
		dm_config_free(this->config);
		this->config = NULL;
	}
	free(this->ring_data);
	this->ring_data = NULL;
	pthread_mutex_destroy(&this->lock);
	pthread_cond_destroy(&this->cond);
	return 0;
}

static size_t impl_get_size(const struct spa_handle_factory *factory,
		const struct spa_dict *params)
{
	return sizeof(struct impl);
}

static int impl_init(const struct spa_handle_factory *factory,
		struct spa_handle *handle,
		const struct spa_dict *info,
		const struct spa_support *support,
		uint32_t n_support)
{
	struct impl *this;
	struct port *port;
	const char *str;

	spa_return_val_if_fail(factory != NULL && handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;
	/* The factory in use decides which direction this is. */
	this->capture = spa_streq(factory->name, "api.droid.pcm.source");
	this->dir = this->capture ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
	if (!this->data_loop || !this->data_system) {
		spa_log_error(this->log, NAME " DataLoop/DataSystem missing from the support array");
		return -EINVAL;
	}

	str = getenv("SPA_DROID_DIAG");
	this->diag = str && spa_atob(str);

	this->timer_source.func = on_timeout;
	this->timer_source.data = this;
	this->timer_source.fd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
	if (this->timer_source.fd < 0) {
		spa_log_error(this->log, NAME " no timer - the node would never "
				"run a cycle");
		return this->timer_source.fd;
	}
	this->timer_source.mask = SPA_IO_IN;
	this->timer_source.rmask = 0;
	spa_loop_add_source(this->data_loop, &this->timer_source);
	this->timer_added = true;

	spa_hook_list_init(&this->hooks);
	this->node.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Node, SPA_VERSION_NODE, &impl_node, this);

	this->info = SPA_NODE_INFO_INIT();
	if (this->capture)
		this->info.max_output_ports = MAX_PORTS;
	else
		this->info.max_input_ports = MAX_PORTS;
	this->info.flags = SPA_NODE_FLAG_RT;
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(SPA_PARAM_Props, SPA_PARAM_INFO_WRITE);
	this->info.params = this->params;
	this->info.n_params = 2;

	port = &this->port;
	port->id = 0;
	port->info = SPA_PORT_INFO_INIT();
	port->info.flags = SPA_PORT_FLAG_NO_REF;
	if (this->capture)
		port->info.flags |= SPA_PORT_FLAG_LIVE | SPA_PORT_FLAG_PHYSICAL |
				    SPA_PORT_FLAG_TERMINAL;
	port->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumFormat, SPA_PARAM_INFO_READ);
	port->params[1] = SPA_PARAM_INFO(SPA_PARAM_Format, SPA_PARAM_INFO_WRITE);
	port->params[2] = SPA_PARAM_INFO(SPA_PARAM_Buffers, SPA_PARAM_INFO_READ);
	port->params[3] = SPA_PARAM_INFO(SPA_PARAM_IO, SPA_PARAM_INFO_READ);
	port->params[4] = SPA_PARAM_INFO(SPA_PARAM_Latency, SPA_PARAM_INFO_READ);
	port->info.params = port->params;
	port->info.n_params = 5;

	/* Which mix port? The primary output or input by default. */
	str = info ? spa_dict_lookup(info, "droid.mix-port") : NULL;
	snprintf(this->mix_port_name, sizeof(this->mix_port_name), "%s",
			str ? str : (this->capture ? "primary input" : "primary output"));

	/* Capture source: the built-in microphone by default, redirectable to a
	 * named device port via droid.device-port (e.g. "Wired Headset Mic"). */
	this->input_device = AUDIO_DEVICE_IN_BUILTIN_MIC;
	str = info ? spa_dict_lookup(info, "droid.device-port") : NULL;
	if (str)
		snprintf(this->input_port_name, sizeof(this->input_port_name), "%s", str);

	/* Preferred format from the node properties. The VoIP channel runs at
	 * 16 kHz, the primary one at 48. */
	this->pref_rate = DEFAULT_RATE;
	this->pref_channels = DEFAULT_CHANNELS;
	if (info) {
		if ((str = spa_dict_lookup(info, "audio.rate")))
			spa_atou32(str, &this->pref_rate, 10);
		if ((str = spa_dict_lookup(info, "audio.channels")))
			spa_atou32(str, &this->pref_channels, 10);
	}
	if (this->pref_rate < 8000 || this->pref_rate > 48000)
		this->pref_rate = DEFAULT_RATE;
	if (this->pref_channels < 1 || this->pref_channels > 2)
		this->pref_channels = DEFAULT_CHANNELS;

	/* Vendor options for the HAL module. See hal_open() for the default. */
	str = info ? spa_dict_lookup(info, "droid.hw-options") : NULL;
	snprintf(this->hw_options, sizeof(this->hw_options), "%s",
			str ? str : "speaker_before_voice=true");

	/* Android audio source. "mic" is what the HAL expects for the built-in
	 * microphone; telephony would use "voice_call" or "voice communication".
	 * Empty leaves AUDIO_SOURCE_DEFAULT in place. */
	str = info ? spa_dict_lookup(info, "droid.audio-source") : NULL;
	snprintf(this->audio_source, sizeof(this->audio_source), "%s", str ? str : "mic");

	str = info ? spa_dict_lookup(info, "droid.config") : NULL;
	snprintf(this->config_file, sizeof(this->config_file), "%s",
			str ? str : "/android/vendor/etc/audio_policy_configuration.xml");
	this->config = pa_parse_droid_audio_config(this->config_file);
	if (!this->config) {
		spa_log_error(this->log, NAME " HAL configuration not readable");
		return -EIO;
	}
	if (!(this->module = dm_config_find_module(this->config, "primary"))) {
		dm_config_free(this->config);
		this->config = NULL;
		return -ENOENT;
	}

	/* Load the HAL module now, while the process is still young.
	 *
	 * libhybris brings its own Android linker, and that linker wants a
	 * particular region of the address space. Opened late - after the
	 * Bluetooth codecs have been loaded, say - it does not get it and faults
	 * inside android_linker_init(), taking the whole daemon down. That is how
	 * playback falling back from a headset to the phone killed the sound:
	 * the process had played over Bluetooth all along and reached for the HAL
	 * for the very first time at the worst possible moment.
	 *
	 * So the first node to be created opens the module and never gives it
	 * back (see the keepalive below). The module is not the exclusive part -
	 * the stream is, and that one is still opened only when something plays. */
	if (hw_module_keepalive == NULL) {
		char args[512];
		pa_modargs *ma;

		snprintf(args, sizeof(args), "config=%s %s",
				this->config_file, this->hw_options);
		if ((ma = pa_modargs_new(args, NULL)) != NULL) {
			pa_droid_hw_module *hw;
			hw = pa_droid_hw_module_get2(pa_compat_core(), ma, "primary");
			pa_modargs_free(ma);
			if (hw != NULL)
				hw_module_keepalive = hw;
			else
				spa_log_warn(this->log, NAME
						" HAL module not loadable up front");
		}
	}

	this->ring_data = malloc(RING_SIZE);
	if (!this->ring_data) {
		dm_config_free(this->config);
		this->config = NULL;
		return -ENOMEM;
	}
	spa_ringbuffer_init(&this->ring);
	pthread_mutex_init(&this->lock, NULL);
	pthread_cond_init(&this->cond, NULL);

	registry_add(this);
	spa_log_info(this->log, NAME " ready for mix port \"%s\"", this->mix_port_name);
	return 0;
}

/* Finds the device port for a PulseAudio route name ("output-earpiece"). The
 * device reports routes under those names while the HAL only knows its own
 * ("Earpiece") - this is where they are translated. */
static dm_config_port *port_by_route_name(struct impl *this, const char *route)
{
	dm_config_module *module = this->hw ? this->hw->enabled_module : this->module;
	dm_config_port *port;
	void *state;

	if (!module)
		return NULL;

	for (port = dm_list_first_data(module->device_ports, &state); port;
	     port = dm_list_next_data(module->device_ports, &state)) {
		const char *name = NULL;
		bool ok = port->role == DM_CONFIG_ROLE_SINK
			? pa_droid_output_port_name(port->type, &name)
			: pa_droid_input_port_name(port->type, &name);
		if (ok && spa_streq(name, route))
			return port;
	}

	/* Bluetooth is missing from this device's audio_policy XML - the whole
	 * section is commented out, lines 167-217, and takes every BT SCO and
	 * A2DP device port with it. The HAL does not read that file; when
	 * routing it only gets the device type. So we add the port ourselves,
	 * once, into the module's own lists.
	 *
	 * Registering it there rather than keeping it aside matters:
	 * pa_droid_open_output_stream() checks the port it is handed against
	 * dm_config_find_device_port(), which searches exactly those lists. A
	 * port that only we know about is refused, and the stream never opens.
	 *
	 * Ownership follows the config's own rule - module->ports frees the
	 * ports, device_ports is a view - so the allocation has to match. */
	{
		static const struct {
			const char *route;
			const char *name;
			audio_devices_t type;
			dm_config_role_t role;
		} missing[] = {
			{ "output-bluetooth_sco",        "BT SCO",
			  AUDIO_DEVICE_OUT_BLUETOOTH_SCO,        DM_CONFIG_ROLE_SINK },
			{ "input-bluetooth_sco_headset", "BT SCO Headset Mic",
			  AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET, DM_CONFIG_ROLE_SOURCE },
		};
		size_t i;

		for (i = 0; i < SPA_N_ELEMENTS(missing); i++) {
			dm_config_port *p;

			if (!spa_streq(route, missing[i].route))
				continue;

			p = pa_xnew0(dm_config_port, 1);
			p->module = module;
			p->port_type = DM_CONFIG_TYPE_DEVICE_PORT;
			p->name = pa_xstrdup(missing[i].name);
			p->address = pa_xstrdup("");
			p->role = missing[i].role;
			p->type = missing[i].type;
			p->profiles = dm_list_new();

			dm_list_push_back(module->ports, p);
			dm_list_push_back(module->device_ports, p);
			spa_log_info(this->log, NAME " \"%s\" is missing from the audio "
					"policy configuration - added it", p->name);
			return p;
		}
	}

	return NULL;
}

/* Apply a route. While the HAL is still closed the request is only
 * remembered and applied at the next hal_open(). */
static int apply_route(struct impl *this, const char *route)
{
	dm_config_port *dev;
	int res;

	if (!(dev = port_by_route_name(this, route))) {
		spa_log_warn(this->log, NAME " route \"%s\" is unknown to the HAL", route);
		return -ENOENT;
	}

	snprintf(this->wanted_port, sizeof(this->wanted_port), "%s", dev->name);
	snprintf(this->wanted_route, sizeof(this->wanted_route), "%s", route);

	if (!this->stream) {
		spa_log_info(this->log, NAME " route \"%s\" remembered (HAL still closed)", dev->name);
		return 0;
	}

	if (this->capture)
		res = pa_droid_hw_set_input_device(this->stream, dev) ? 0 : -EIO;
	else if (!pa_droid_stream_is_primary(this->stream))
		return 0;   /* see hal_open(): only the primary stream routes */
	else
		res = pa_droid_stream_set_route(this->stream, dev);

	if (res < 0)
		spa_log_warn(this->log, NAME " route \"%s\" failed: %d", dev->name, res);
	else
		DIAG(this, "route changed: %s -> %s", route, dev->name);
	return res;
}

/* Volume during a call. No PCM flows through the graph while a call is up, so
 * the adapter's software gain has nothing to act on. The voice path's level
 * lives in the HAL and is set through set_voice_volume; PulseAudio's
 * droid-sink does the same in its call profile. */
static int apply_voice_volume(struct impl *this, const char *value)
{
	float vol;

	if (this->capture || !this->hw)
		return 0;
	if (!this->in_call)
		return 0;   /* outside a call the HAL does nothing with it */

	/* NOT atof(): in a locale that uses a comma as the decimal separator it
	 * does not read the dot - "0.343" became 0.00 and the voice level dropped
	 * to zero. spa_atof switches to the C locale internally. */
	if (!spa_atof(value, &vol)) {
		spa_log_warn(this->log, NAME " voice volume \"%s\" not readable", value);
		return -EINVAL;
	}
	if (vol < 0.0f)
		vol = 0.0f;
	else if (vol > 1.0f)
		vol = 1.0f;

	pa_droid_hw_module_lock(this->hw);
	if (this->hw->device->set_voice_volume) {
		int r = this->hw->device->set_voice_volume(this->hw->device, vol);
		if (r < 0)
			spa_log_warn(this->log, NAME " voice volume %.2f rejected (%d)", vol, r);
		else
			DIAG(this, "voice volume: %.2f", vol);
	} else {
		spa_log_warn(this->log, NAME " HAL offers no set_voice_volume");
	}
	pa_droid_hw_module_unlock(this->hw);
	return 0;
}

/* Call mode. The mode belongs to the HAL module, not to the stream - but
 * pa_droid_hw_set_mode needs the primary output stream in order to route to
 * the speaker first and the earpiece afterwards when switching to
 * AUDIO_MODE_IN_CALL (some devices start the call wrong otherwise). That is
 * why the HAL is opened here if need be - PulseAudio does the same with a
 * virtual stream (voice_virtual_stream). */
/* Tell the HAL again which audio source we want.
 *
 * When a call starts the HAL takes the mix port's source for itself
 * ("overriding audio source mic with voice call") and does not give it back
 * when the call ends. Everything recording afterwards reads digital silence -
 * measured: 48000 samples, every one of them zero, against RMS 633 and 4287
 * distinct values from the same microphone a moment later. Nothing short of
 * reopening the stream fixed it, so the source has to be set again by hand. */
static void reapply_audio_source(struct impl *this)
{
	pa_sample_spec spec;
	pa_channel_map map;
	pa_proplist *pl;

	if (!this->stream || !this->audio_source[0])
		return;

	spec.format = PA_SAMPLE_S16LE;
	spec.rate = this->port.have_format
		? this->port.current_format.info.raw.rate : DEFAULT_RATE;
	spec.channels = this->port.have_format
		? this->port.current_format.info.raw.channels : DEFAULT_CHANNELS;
	if (spec.channels == 1)
		pa_channel_map_init_mono(&map);
	else
		pa_channel_map_init_stereo(&map);

	pl = pa_proplist_new();
	pa_proplist_sets(pl, EXT_PROP_AUDIO_SOURCE, this->audio_source);
	if (!pa_droid_stream_reconfigure_input(this->stream, &spec, &map, pl))
		spa_log_warn(this->log, NAME " audio source \"%s\" could not be restored",
				this->audio_source);
	else
		DIAG(this, "audio source back to %s", this->audio_source);
	pa_proplist_free(pl);
}

static int apply_mode(struct impl *this, const char *mode)
{
	audio_mode_t m;
	int res;

	if (spa_streq(mode, "call"))
		m = AUDIO_MODE_IN_CALL;
	else if (spa_streq(mode, "communication"))
		m = AUDIO_MODE_IN_COMMUNICATION;
	else if (spa_streq(mode, "ringtone"))
		m = AUDIO_MODE_RINGTONE;
	else
		m = AUDIO_MODE_NORMAL;

	/* The mode itself is the playback node's business - it holds the primary
	 * output. The capture node only has to undo what the call did to its
	 * audio source, see reapply_audio_source(). */
	if (this->capture) {
		if (m == AUDIO_MODE_NORMAL)
			reapply_audio_source(this);
		return 0;
	}

	if (m != AUDIO_MODE_NORMAL) {
		if ((res = hal_open(this)) < 0) {
			spa_log_warn(this->log, NAME " call mode: could not open the HAL");
			return res;
		}
		this->mode_holds_hal = true;
	}

	if (!this->hw)
		return 0;   /* nothing open, nothing to do */

	if (!pa_droid_hw_set_mode(this->hw, m)) {
		spa_log_warn(this->log, NAME " audio mode \"%s\" rejected", mode);
		return -EIO;
	}
	this->in_call = m == AUDIO_MODE_IN_CALL;

	/* On MediaTek "realcall" unlocks the real voice path in the DSP - and
	 * with it the DSP's echo cancellation. PulseAudio's card module sends it
	 * when switching to the call profile; we never ported that module, so we
	 * do it here. Only when the option is set - this particular MediaTek HAL
	 * rejects the parameter anyway (-22), it is apparently a Qualcomm legacy.
	 * The code stays for other devices. */
	if (pa_droid_option(this->hw, DM_OPTION_REALCALL)) {
		const char *param = this->in_call ? "realcall=on" : "realcall=off";
		if (pa_droid_set_parameters(this->hw, param) < 0)
			spa_log_warn(this->log, NAME " HAL rejects \"%s\"", param);
		else
			DIAG(this, "%s sent to the HAL", param);
	}
	DIAG(this, "audio mode: %s", mode);

	/* On entering a call the HAL routes to the earpiece by itself. The card
	 * knows nothing of that and still believes in its own route - switching
	 * to exactly that route later would then be a no-op, leaving HAL and card
	 * permanently out of step. Hence re-apply the route the device asked for
	 * last. */
	if (this->wanted_port[0] && this->stream &&
	    pa_droid_stream_is_primary(this->stream)) {
		dm_config_port *dev = dm_config_find_port(this->hw->enabled_module,
				this->wanted_port);
		if (dev)
			pa_droid_stream_set_route(this->stream, dev);
	}

	/* Call over and nothing to play: release the hardware again. */
	if (m == AUDIO_MODE_NORMAL && this->mode_holds_hal) {
		this->mode_holds_hal = false;
		if (!this->started)
			hal_close(this);
	}
	return 0;
}

/* Called by the device: switch the route to a named device port. While the
 * HAL is not open yet the request is only remembered and applied at the next
 * hal_open(). */
int droid_node_set_route(const char *mix_port, const char *device_port);

int droid_node_set_route(const char *mix_port, const char *device_port)
{
	struct impl *this = NULL;
	int res = -ENOENT;
	unsigned i;

	pthread_mutex_lock(&registry.lock);
	for (i = 0; i < MAX_REG; i++) {
		if (registry.e[i].node && spa_streq(registry.e[i].mix_port, mix_port)) {
			this = registry.e[i].node;
			break;
		}
	}
	pthread_mutex_unlock(&registry.lock);

	if (!this)
		return -ENOENT;

	res = apply_route(this, device_port);
	return res;
}

static void registry_add(struct impl *this)
{
	unsigned i;
	pthread_mutex_lock(&registry.lock);
	for (i = 0; i < MAX_REG; i++) {
		if (!registry.e[i].node) {
			registry.e[i].mix_port = this->mix_port_name;
			registry.e[i].node = this;
			break;
		}
	}
	pthread_mutex_unlock(&registry.lock);
}

static void registry_remove(struct impl *this)
{
	unsigned i;
	pthread_mutex_lock(&registry.lock);
	for (i = 0; i < MAX_REG; i++)
		if (registry.e[i].node == this)
			registry.e[i].node = NULL;
	pthread_mutex_unlock(&registry.lock);
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Node, },
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL && info != NULL && index != NULL, -EINVAL);
	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;
	*info = &impl_interfaces[(*index)++];
	return 1;
}

const struct spa_handle_factory droid_pcm_source_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	.name = "api.droid.pcm.source",
	.info = NULL,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};

const struct spa_handle_factory droid_pcm_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	.name = "api.droid.pcm",
	.info = NULL,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};
