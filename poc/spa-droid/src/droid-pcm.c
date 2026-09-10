/* SPA-Node: Wiedergabe und Aufnahme ueber den Android-Audio-HAL.
 *
 * Der HAL-Write blockiert, bis die Daten abgenommen sind. Er darf deshalb
 * nicht im Datenthread des Graphen laufen. Aufbau daher:
 *
 *   process()        -> schreibt in einen Ringpuffer  (Graph-Thread)
 *   writer_thread()  -> liest daraus, ruft pa_droid_stream_write (eigener Thread)
 *
 * Aufnahme ist dasselbe rueckwaerts:
 *
 *   reader_thread()  -> pa_droid_stream_read, legt in den Ringpuffer
 *   process()        -> holt daraus, reicht den Puffer an den Graphen
 *
 * Beide Richtungen teilen sich diesen Code; welche es ist, entscheidet die
 * benutzte Factory (api.droid.pcm bzw. api.droid.pcm.source).
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

/* Das Device (droid-device.c) und der Node sind getrennte SPA-Objekte, leben
 * aber im selben Prozess. Damit eine Routenaenderung am Device den laufenden
 * HAL-Stream erreicht, tragen sich Nodes hier ein. Bewusst winzig: mehr als
 * eine Handvoll Nodes gibt es nicht. */
#define MAX_REG 8
static struct {
	pthread_mutex_t lock;
	struct { const char *mix_port; struct impl *node; } e[MAX_REG];
} registry = { .lock = PTHREAD_MUTEX_INITIALIZER };

struct impl;
static void registry_add(struct impl *this);
static void registry_remove(struct impl *this);

#define MAX_PORTS       1
#define RING_SIZE       (1u << 18)   /* 256 kB, Zweierpotenz fuer spa_ringbuffer */
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

	/* Richtung: Wiedergabe hat einen Eingangsport, Aufnahme einen Ausgang. */
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
	char input_port_name[64];   /* leer = ueber den Geraetetyp suchen */
	char audio_source[32];      /* Android-Audioquelle, z. B. "mic" */
	char wanted_port[64];       /* vom Device gewuenschte Route */
	bool mode_holds_hal;        /* HAL nur wegen Anrufmodus offen */
	bool in_call;               /* Modus ist AUDIO_MODE_IN_CALL */

	/* Uebergabe an den Schreib-Thread */
	struct spa_ringbuffer ring;
	uint8_t *ring_data;
	pthread_t writer;
	pthread_mutex_t lock;
	pthread_cond_t cond;
	bool running;
	bool started;
	bool drain;

	/* Taktgeber: ein Hardware-Sink treibt den Graphen selbst */
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

	/* Instrumentierung des Datenpfads */
	bool diag;               /* ausfuehrliche Diagnose (SPA_DROID_DIAG=1) */
	uint64_t bytes_queued;   /* von process() in den Ring gelegt */
	uint64_t bytes_written;  /* vom writer_thread an den HAL gegeben */
	uint32_t n_process;
	uint32_t n_write;
	uint32_t n_write_err;    /* abgewiesene HAL-Writes bzw. -Reads */
	uint32_t n_overrun;      /* verworfene Bloecke, weil der Ring voll war */
	uint32_t n_underrun;     /* Aufnahme: mit Stille aufgefuellte Bloecke */
};

/* Diagnose: standardmaessig auf info (unter PipeWires Loglevel unsichtbar),
 * mit SPA_DROID_DIAG=1 auf warn, damit man sie ohne PIPEWIRE_DEBUG sieht. */
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

/* ------------------------------------------------------------------ HAL */

static int hal_open_input(struct impl *this, const pa_sample_spec *spec,
		const pa_channel_map *map)
{
	const pa_sample_spec *got;
	dm_config_port *mix, *dev;

	/* Anders als beim Ausgang nimmt der Eingang den mixPort als NAME - die
	 * Zeigeridentitaets-Falle gibt es hier also nicht. */
	this->stream = pa_droid_open_input_stream(this->hw, spec, map, this->mix_port_name);
	if (!this->stream) {
		spa_log_error(this->log, NAME " Eingabestream \"%s\" fehlgeschlagen",
				this->mix_port_name);
		return -EIO;
	}

	/* Der HAL bekommt beim blossen Oeffnen AUDIO_SOURCE_DEFAULT. Android-HALs
	 * haengen ihre Mikrofonaufbereitung (Verstaerkung, Rauschunterdrueckung,
	 * Echokompensation) aber an der Audioquelle. reconfigure_input setzt sie
	 * und oeffnet den Stream dabei selbst neu. */
	if (this->audio_source[0]) {
		pa_proplist *pl = pa_proplist_new();
		pa_proplist_sets(pl, EXT_PROP_AUDIO_SOURCE, this->audio_source);
		if (!pa_droid_stream_reconfigure_input(this->stream, spec, map, pl))
			spa_log_warn(this->log, NAME " Audioquelle \"%s\" liess sich nicht setzen",
					this->audio_source);
		else
			DIAG(this, "Audioquelle: %s", this->audio_source);
		pa_proplist_free(pl);
	}

	/* Der HAL darf Rate und Kanalzahl beim Oeffnen aendern. Unser Port hat
	 * aber schon ein ausgehandeltes Format - eine Abweichung wuerde
	 * unbemerkt Tonhoehe und Kanalzuordnung verbiegen. Also lieber ehrlich
	 * scheitern und die tatsaechlichen Werte melden. */
	got = pa_droid_stream_sample_spec(this->stream);
	if (got->rate != spec->rate || got->channels != spec->channels ||
	    got->format != spec->format) {
		spa_log_error(this->log, NAME " HAL lieferte anderes Format als ausgehandelt: "
				"%u Hz/%u Kanaele/Format %d statt %u Hz/%u Kanaele/Format %d",
				got->rate, got->channels, got->format,
				spec->rate, spec->channels, spec->format);
		pa_droid_stream_unref(this->stream);
		this->stream = NULL;
		return -EINVAL;
	}

	mix = dm_config_find_mix_port(this->hw->enabled_module, this->mix_port_name);
	if (this->wanted_port[0])
		dev = dm_config_find_port(this->hw->enabled_module, this->wanted_port);
	else if (this->input_port_name[0])
		dev = dm_config_find_port(this->hw->enabled_module, this->input_port_name);
	else
		dev = mix ? dm_config_find_device_port(mix, this->input_device) : NULL;
	if (!dev)
		spa_log_warn(this->log, NAME " Eingabegeraet %#x nicht gefunden - "
				"HAL behaelt sein aktuelles Routing", this->input_device);
	else if (!pa_droid_hw_set_input_device(this->stream, dev))
		spa_log_warn(this->log, NAME " Routing auf \"%s\" fehlgeschlagen", dev->name);
	else
		DIAG(this, "Eingabegeraet gesetzt: %s", dev->name);

	latency_changed(this);
	spa_log_info(this->log, NAME " Aufnahmestream offen: %s, %u Hz, %u Kanaele, Puffer %zu B",
			this->mix_port_name, spec->rate, spec->channels,
			pa_droid_stream_buffer_size(this->stream));
	return 0;
}

static int hal_open(struct impl *this)
{
	dm_config_port *mix, *dev;
	pa_sample_spec spec;
	pa_channel_map map;

	if (this->stream)
		return 0;

	if (!(this->hw = pa_droid_hw_module_get(pa_compat_core(), this->config, "primary"))) {
		spa_log_error(this->log, NAME " HAL-Modul liess sich nicht oeffnen");
		return -EIO;
	}

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
		return 0;
	}

	/* WICHTIG: pa_droid_hw_module_get dupliziert die Konfiguration
	 * (dm_config_dup). pa_droid_open_output_stream vergleicht die Ports per
	 * ZEIGERIDENTITAET gegen hw->enabled_module. Ports aus unserer eigenen
	 * Kopie werden deshalb immer abgelehnt - sie muessen aus dem HAL-Modul
	 * stammen. */
	mix = dm_config_find_mix_port(this->hw->enabled_module, this->mix_port_name);
	dev = this->wanted_port[0]
		? dm_config_find_port(this->hw->enabled_module, this->wanted_port)
		: NULL;
	if (!dev)
		dev = dm_config_default_output_device(this->hw->enabled_module);
	if (!mix || !dev) {
		spa_log_error(this->log, NAME " mixPort \"%s\" oder Standardausgabe fehlt",
				this->mix_port_name);
		pa_droid_hw_module_unref(this->hw);
		this->hw = NULL;
		return -ENOENT;
	}

	spec.format = PA_SAMPLE_S16LE;
	spec.rate = this->port.have_format
		? this->port.current_format.info.raw.rate : DEFAULT_RATE;
	spec.channels = this->port.have_format
		? this->port.current_format.info.raw.channels : DEFAULT_CHANNELS;
	if (spec.channels == 1)
		pa_channel_map_init_mono(&map);
	else
		pa_channel_map_init_stereo(&map);

	this->stream = pa_droid_open_output_stream(this->hw, &spec, &map, mix, dev);
	if (!this->stream) {
		spa_log_error(this->log, NAME " Ausgabestream \"%s\" -> \"%s\" fehlgeschlagen",
				mix->name, dev->name);
		pa_droid_hw_module_unref(this->hw);
		this->hw = NULL;
		return -EIO;
	}

	latency_changed(this);
	spa_log_info(this->log, NAME " Stream offen: %s -> %s, %u Hz, %u Kanaele, Puffer %zu B",
			mix->name, dev->name, spec.rate, spec.channels,
			pa_droid_stream_buffer_size(this->stream));

	/* Ohne diese beiden Schritte oeffnet der Stream zwar, bleibt aber stumm.
	 * PulseAudios droid-sink macht genau dasselbe (do_routing/update_volumes). */
	if (pa_droid_stream_set_route(this->stream, dev) < 0)
		spa_log_warn(this->log, NAME " Routing auf \"%s\" fehlgeschlagen", dev->name);
	else
		DIAG(this, "Routing gesetzt: %s", dev->name);

	pa_droid_hw_module_lock(this->hw);
	if (this->stream->output->stream->set_volume) {
		int r = this->stream->output->stream->set_volume(
				this->stream->output->stream, 1.0f, 1.0f);
		DIAG(this, "HAL-Lautstaerke auf 1.0 (ret %d)", r);
	} else {
		spa_log_warn(this->log, NAME " HAL bietet kein set_volume - Pegel bleibt HAL-Standard");
	}
	pa_droid_hw_module_unlock(this->hw);

	this->bytes_queued = this->bytes_written = 0;
	this->n_process = this->n_write = 0;
	this->n_write_err = this->n_overrun = this->n_underrun = 0;
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
	/* Ohne Stream gibt es keine Verzoegerung mehr - sonst bliebe der alte
	 * Wert stehen und der Graph rechnete mit einer Latenz, die es nicht
	 * mehr gibt. */
	if (had_stream)
		latency_changed(this);
}

/* Wie weit hinkt der Ton der Anzeige hinterher?
 *
 * PipeWire kann das nicht erraten: es kennt weder die Puffer des HAL noch
 * unseren Ringpuffer dazwischen. Ohne Meldung nimmt es null an - dann laeuft
 * bei Video der Ton dem Bild voraus. Gemeldet wird die Summe aus beidem. */
static uint64_t latency_ns(struct impl *this)
{
	uint32_t stride, idx;
	uint64_t ns = 0;
	int32_t avail;

	if (this->stream)
		ns = (uint64_t) pa_droid_stream_get_latency(this->stream) * 1000;

	stride = 2 * (this->port.have_format
			? this->port.current_format.info.raw.channels : DEFAULT_CHANNELS);
	avail = this->capture
		? spa_ringbuffer_get_read_index(&this->ring, &idx)
		: spa_ringbuffer_get_read_index(&this->ring, &idx);
	if (avail > 0 && this->rate && stride)
		ns += (uint64_t) avail * SPA_NSEC_PER_SEC / ((uint64_t) this->rate * stride);

	return ns;
}

/* Die Latenz aendert sich, wenn der HAL-Stream aufgeht - dann muss sie neu
 * gemeldet werden. Das SERIAL-Bit kippt, sonst merkt es niemand. */
static void latency_changed(struct impl *this)
{
	uint32_t i;
	for (i = 0; i < SPA_N_ELEMENTS(this->port.params); i++)
		if (this->port.params[i].id == SPA_PARAM_Latency)
			this->port.params[i].flags ^= SPA_PARAM_INFO_SERIAL;
	this->port.info.change_mask |= SPA_PORT_CHANGE_MASK_PARAMS;
	emit_port_info(this, &this->port, false);
}

/* ------------------------------------------------------- Taktgeber */

static void set_timeout(struct impl *this, uint64_t time)
{
	struct itimerspec ts;

	ts.it_value.tv_sec  = time / SPA_NSEC_PER_SEC;
	ts.it_value.tv_nsec = time % SPA_NSEC_PER_SEC;
	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	spa_system_timerfd_settime(this->data_system, this->timer_source.fd,
			SPA_FD_TIMER_ABSTIME, &ts, NULL);
}

static void timer_stop(struct impl *this)
{
	struct itimerspec ts = { { 0, 0 }, { 0, 0 } };
	if (this->data_system && this->timer_source.fd >= 0)
		spa_system_timerfd_settime(this->data_system, this->timer_source.fd, 0, &ts, NULL);
}

/* Pro Tick laeuft der Graph einmal - erst dadurch wird process() aufgerufen. */
static void on_timeout(struct spa_source *source)
{
	struct impl *this = source->data;
	uint64_t expirations, nsec;

	if (spa_system_timerfd_read(this->data_system, this->timer_source.fd, &expirations) < 0)
		return;

	nsec = this->next_time;

	/* Die Quantum-Groesse gehoert dem Graphen, nicht uns. Sie muss nicht zur
	 * HAL-Puffergroesse passen - der Ringpuffer entkoppelt beides. Ohne diese
	 * Anpassung liefe der Takt weiter mit dem Startwert, waehrend der Graph
	 * schon eine andere Groesse verarbeitet. */
	if (this->position && this->position->clock.target_duration &&
	    this->position->clock.target_duration != this->quantum && this->rate) {
		this->quantum = this->position->clock.target_duration;
		this->period_ns = (uint64_t) this->quantum * SPA_NSEC_PER_SEC / this->rate;
		DIAG(this, "Quantum vom Graphen geaendert: %u Frames alle %llu us",
				this->quantum, (unsigned long long) (this->period_ns / 1000));
	}

	if (this->clock) {
		this->clock->nsec = nsec;
		this->clock->rate = this->clock->target_rate;
		this->clock->position += this->clock->duration;
		this->clock->duration = this->quantum;
		/* In Abtastwerten, wie PipeWire es erwartet. */
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

	/* Periode aus der HAL-Puffergroesse: buffer_size / (2 Byte * Kanaele) Frames */
	this->rate = rate;
	this->quantum = bufsz ? (uint32_t) (bufsz / (2 * channels)) : 1024;
	if (this->quantum == 0)
		this->quantum = 1024;
	if (this->position && this->position->clock.target_duration)
		this->quantum = this->position->clock.target_duration;
	this->period_ns = (uint64_t) this->quantum * SPA_NSEC_PER_SEC / rate;

	spa_system_clock_gettime(this->data_system, CLOCK_MONOTONIC, &now);
	this->next_time = SPA_TIMESPEC_TO_NSEC(&now) + this->period_ns;
	set_timeout(this, this->next_time);

	DIAG(this, "Taktgeber laeuft: %u Frames alle %llu us",
			this->quantum, (unsigned long long) (this->period_ns / 1000));
	return 0;
}

/* ------------------------------------------------- Schreib-Thread */

static void *writer_thread(void *arg)
{
	struct impl *this = arg;
	size_t chunk = pa_droid_stream_buffer_size(this->stream);
	uint8_t *buf;

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
			/* Beim Anhalten den Rest nicht liegen lassen: was noch im Ring
			 * steht, wird mit Stille auf einen vollen HAL-Puffer aufgefuellt
			 * und ausgeschrieben. Sonst fehlen die letzten ~21 ms. */
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
			DIAG(this, "Rest ausgeschrieben: %zu B Daten + %zu B Stille",
					take, chunk - take);
		}

		{
			ssize_t w = pa_droid_stream_write(this->stream, buf, chunk);
			if (w < 0) {
				/* Nicht pro Quantum protokollieren - im Dauerfehlerfall
				 * waere das ein Log-Sturm. Erster Fehler + Bilanz reichen. */
				if (this->n_write_err++ == 0)
					spa_log_warn(this->log, NAME " HAL-Write fehlgeschlagen: %zd "
							"(weitere werden nur gezaehlt)", w);
			} else {
				if (this->n_write == 0)
					DIAG(this, "erster HAL-Write ok: %zd von %zu B", w, chunk);
				this->bytes_written += (uint64_t) w;
				this->n_write++;
			}
		}
	}

	free(buf);
	return NULL;
}

/* Aufnahme: pa_droid_stream_read blockiert bis zur naechsten HAL-Periode und
 * gibt damit den Takt vor. Deshalb ebenfalls ein eigener Thread. */
static void *reader_thread(void *arg)
{
	struct impl *this = arg;
	size_t chunk = pa_droid_stream_buffer_size(this->stream);
	uint8_t *buf;

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
				spa_log_warn(this->log, NAME " HAL-Read fehlgeschlagen: %zd "
						"(weitere werden nur gezaehlt)", r);
			/* Nicht heisslaufen, wenn der HAL dauerhaft sofort scheitert. */
			usleep((useconds_t) (this->period_ns / 1000));
			continue;
		}

		if (this->n_write == 0)
			DIAG(this, "erster HAL-Read ok: %zd von %zu B", r, chunk);
		this->bytes_written += (uint64_t) r;
		this->n_write++;

		filled = spa_ringbuffer_get_write_index(&this->ring, &idx);
		if (filled + r > (int32_t) RING_SIZE) {
			/* Niemand holt die Daten ab - lieber die aeltesten wegwerfen als
			 * die neuesten, sonst laeuft die Aufnahme immer weiter hinterher. */
			if (this->n_overrun++ == 0)
				spa_log_warn(this->log, NAME " Ringpuffer voll, Aufnahme verwirft "
						"aelteste Daten (weitere werden nur gezaehlt)");
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
	/* Ring leeren: ein abgebrochener Lauf darf den naechsten nicht mit
	 * altem Material beginnen lassen. Der Graph laeuft hier noch nicht. */
	spa_ringbuffer_init(&this->ring);
	this->running = true;
	this->drain = true;
	if (pthread_create(&this->writer, NULL,
				this->capture ? reader_thread : writer_thread, this) != 0) {
		this->running = false;
		return -errno;
	}
	this->started = true;
	return 0;
}

/* drain=false verwirft den Rest sofort (Notausstieg), drain=true schreibt ihn
 * mit Stille aufgefuellt noch aus. */
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
	DIAG(this, "Bilanz: process() %ux / %llu B, HAL-%s %ux / %llu B",
			this->n_process, (unsigned long long) this->bytes_queued,
			this->capture ? "Read" : "Write",
			this->n_write, (unsigned long long) this->bytes_written);
	if (this->n_underrun)
		DIAG(this, "%u Bloecke mit Stille aufgefuellt (Ring war leer)", this->n_underrun);
	if (this->n_write_err || this->n_overrun)
		spa_log_warn(this->log, NAME " Stoerungen im Lauf: %u abgewiesene HAL-%s, "
				"%u verworfene Bloecke (Ring voll)",
				this->n_write_err, this->capture ? "Reads" : "Writes",
				this->n_overrun);
}

/* ------------------------------------------------------------- Node */

static void emit_node_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	/* props MUSS gesetzt sein: libpipewire-module-adapter reicht info->props
	 * ungeprueft an pw_properties_update weiter - NULL segfaultet dort. */
	struct spa_dict_item items[3];
	uint32_t n = 0;

	/* "droid-hal" ist die Kennung von PulseAudios droid-Modul. callaudiod
	 * erkennt eine Android-Karte allein daran. */
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
		/* Emit ist synchron - der Stack-Dict lebt lange genug. */
		spa_node_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
	this->info.props = NULL;
}

static void emit_port_info(struct impl *this, struct port *port, bool full)
{
	uint64_t old = full ? port->info.change_mask : 0;
	struct spa_dict_item items[1];

	/* gleiche Falle wie beim Node: props darf nicht NULL sein */
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
			spa_log_error(this->log, NAME " Start ohne ausgehandeltes Format");
			return -EIO;
		}
		if ((res = hal_open(this)) < 0)
			return res;
		/* Teilzustaende zurueckrollen: sonst bliebe ein offener HAL-Stream
		 * oder ein laufender Thread zurueck, waehrend der Node als
		 * gescheitert gilt. */
		if ((res = writer_start(this)) < 0) {
			spa_log_error(this->log, NAME " Schreib-Thread liess sich nicht starten: %s",
					spa_strerror(res));
			hal_close(this);
			return res;
		}
		if ((res = timer_start(this)) < 0) {
			spa_log_error(this->log, NAME " Taktgeber liess sich nicht starten: %s",
					spa_strerror(res));
			writer_stop(this, false);
			hal_close(this);
			return res;
		}
		DIAG(this, "Start-Kommando erhalten, writer laeuft");
		break;
	case SPA_NODE_COMMAND_Pause:
		timer_stop(this);
		writer_stop(this, true);
		spa_log_info(this->log, NAME " angehalten");
		break;
	case SPA_NODE_COMMAND_Suspend:
		/* Suspend gibt die Hardware frei - erst dadurch kann PulseAudio das
		 * PCM-Geraet wieder bekommen, wenn der Sink nur untaetig herumsteht.
		 * Im Anruf bleibt sie offen: dort laeuft der Sprachpfad ueber Modem
		 * und DSP, ohne dass ein PipeWire-Strom spielt. */
		timer_stop(this);
		writer_stop(this, true);
		if (this->mode_holds_hal) {
			spa_log_info(this->log, NAME " angehalten, HAL bleibt fuer den Anruf offen");
		} else {
			hal_close(this);
			spa_log_info(this->log, NAME " angehalten, HAL freigegeben");
		}
		break;
	default:
		return -ENOTSUP;
	}
	return 0;
}

/* Kanalpositionen gehoeren NICHT hierher: eine Kanalspanne und feste
 * Positionen schliessen sich im selben Format-Objekt aus, und mit zwei festen
 * Varianten handelte der Adapter prompt Mono aus. Die Zuordnung liefert die
 * Knoteneigenschaft audio.position (FL,FR), die das Device mitgibt. */
static int port_enum_formats(struct impl *this, struct spa_pod_builder *b,
		uint32_t index, struct spa_pod **param)
{
	if (index > 0)
		return 0;

	*param = spa_pod_builder_add_object(b,
		SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
		SPA_FORMAT_mediaType,      SPA_POD_Id(SPA_MEDIA_TYPE_audio),
		SPA_FORMAT_mediaSubtype,   SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
		SPA_FORMAT_AUDIO_format,   SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
		SPA_FORMAT_AUDIO_rate,     SPA_POD_CHOICE_RANGE_Int(DEFAULT_RATE, 8000, 48000),
		SPA_FORMAT_AUDIO_channels, SPA_POD_CHOICE_RANGE_Int(DEFAULT_CHANNELS, 1, 2));
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
		/* Der Puffer muss ein GRAPH-Quantum fassen, nicht eine HAL-Periode.
		 * Beim Ausgang sind beide zufaellig gleich gross (4096 B), beim
		 * Eingang nicht: der HAL liefert 3840 B, der Graph will 4096 B.
		 * Ein zu kleiner Puffer laesst den Adapter mit einem Teilquantum
		 * arbeiten. */
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
		/* Ein Sink meldet die Verzoegerung stromabwaerts, eine Quelle die
		 * stromaufwaerts - deshalb die Richtung des eigenen Ports. */
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
		/* Format weg heisst: der Port wird abgebaut. Taktgeber muss mit,
		 * sonst tickt er ohne Datenpfad weiter. */
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

/* Aufnahme: einen freien Puffer nehmen, aus dem Ring fuellen, dem Graphen
 * hinlegen. Ist der Ring leer (Anlauf, Aussetzer), wird mit Stille aufgefuellt -
 * ein zu kurzer Puffer waere fuer den Graphen ein Fehler. */
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
		DIAG(this, "erster process(): %u B ausgeliefert (%u B aus dem Ring)", want, take);
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

	filled = spa_ringbuffer_get_write_index(&this->ring, &idx);
	if (filled + size > RING_SIZE) {
		if (this->n_overrun++ == 0)
			spa_log_warn(this->log, NAME " Ringpuffer voll, %u Bytes verworfen "
					"(weitere werden nur gezaehlt)", size);
	} else {
		spa_ringbuffer_write_data(&this->ring, this->ring_data, RING_SIZE,
				idx & (RING_SIZE - 1),
				SPA_PTROFF(d->data, offs, void), size);
		spa_ringbuffer_write_update(&this->ring, idx + size);
		if (this->n_process == 0)
			DIAG(this, "erster process(): %u B eingereiht", size);
		this->bytes_queued += size;
		this->n_process++;

		pthread_mutex_lock(&this->lock);
		pthread_cond_signal(&this->cond);
		pthread_mutex_unlock(&this->lock);
	}

	io->status = SPA_STATUS_NEED_DATA;
	return SPA_STATUS_NEED_DATA;
}

/* Der einzige Weg, auf dem eine Routenaenderung vom Device hierher findet:
 * Device und Node laufen in verschiedenen Prozessen (Device in WirePlumber,
 * Node im PipeWire-Daemon). WirePlumber schiebt den Routennamen als
 * SPA_PROP_params-Paar herueber. */
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
	/* Welche Richtung, entscheidet die benutzte Factory. */
	this->capture = spa_streq(factory->name, "api.droid.pcm.source");
	this->dir = this->capture ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	this->data_loop = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataLoop);
	this->data_system = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_DataSystem);
	if (!this->data_loop || !this->data_system) {
		spa_log_error(this->log, NAME " DataLoop/DataSystem fehlen im support-Array");
		return -EINVAL;
	}

	str = getenv("SPA_DROID_DIAG");
	this->diag = str && spa_atob(str);

	this->timer_source.func = on_timeout;
	this->timer_source.data = this;
	this->timer_source.fd = spa_system_timerfd_create(this->data_system,
			CLOCK_MONOTONIC, SPA_FD_CLOEXEC | SPA_FD_NONBLOCK);
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

	/* Welcher mixPort? Standard ist der primaere Aus- bzw. Eingang. */
	str = info ? spa_dict_lookup(info, "droid.mix-port") : NULL;
	snprintf(this->mix_port_name, sizeof(this->mix_port_name), "%s",
			str ? str : (this->capture ? "primary input" : "primary output"));

	/* Aufnahmequelle: standardmaessig das eingebaute Mikrofon, per
	 * droid.device-port aber auf einen benannten devicePort umlenkbar
	 * (z. B. "Wired Headset Mic"). */
	this->input_device = AUDIO_DEVICE_IN_BUILTIN_MIC;
	str = info ? spa_dict_lookup(info, "droid.device-port") : NULL;
	if (str)
		snprintf(this->input_port_name, sizeof(this->input_port_name), "%s", str);

	/* Android-Audioquelle. "mic" ist das, was der HAL fuer das eingebaute
	 * Mikrofon erwartet; fuer Telefonie waere es "voice_call" bzw.
	 * "voice_communication". Leer laesst AUDIO_SOURCE_DEFAULT stehen. */
	str = info ? spa_dict_lookup(info, "droid.audio-source") : NULL;
	snprintf(this->audio_source, sizeof(this->audio_source), "%s", str ? str : "mic");

	str = info ? spa_dict_lookup(info, "droid.config") : NULL;
	this->config = pa_parse_droid_audio_config(
			str ? str : "/android/vendor/etc/audio_policy_configuration.xml");
	if (!this->config) {
		spa_log_error(this->log, NAME " HAL-Konfiguration nicht lesbar");
		return -EIO;
	}
	if (!(this->module = dm_config_find_module(this->config, "primary"))) {
		dm_config_free(this->config);
		this->config = NULL;
		return -ENOENT;
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
	spa_log_info(this->log, NAME " bereit fuer mixPort \"%s\"", this->mix_port_name);
	return 0;
}

/* Sucht den devicePort zu einem PulseAudio-Routennamen ("output-earpiece").
 * Das Device meldet Routen unter diesen Namen, der HAL kennt nur seine
 * eigenen ("Earpiece") - hier wird uebersetzt. */
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

	/* Bluetooth fehlt in der audio_policy-XML dieses Geraets (alles
	 * auskommentiert). Der HAL braucht sie nicht - er bekommt beim Routen nur
	 * den Geraetetyp -, also bauen wir den Port selbst. Das Device meldet
	 * dieselben Routen. */
	{
		static dm_config_port bt_out, bt_in;
		dm_config_port *p = NULL;

		if (spa_streq(route, "output-bluetooth_sco")) {
			p = &bt_out;
			p->name = (char *) "BT SCO";
			p->role = DM_CONFIG_ROLE_SINK;
			p->type = AUDIO_DEVICE_OUT_BLUETOOTH_SCO;
		} else if (spa_streq(route, "input-bluetooth_sco_headset")) {
			p = &bt_in;
			p->name = (char *) "BT SCO Headset Mic";
			p->role = DM_CONFIG_ROLE_SOURCE;
			p->type = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
		}
		if (p) {
			p->module = module;
			p->port_type = DM_CONFIG_TYPE_DEVICE_PORT;
			p->address = (char *) "";
			return p;
		}
	}

	return NULL;
}

/* Route anwenden. Ist der HAL noch zu, wird der Wunsch nur gemerkt und beim
 * naechsten hal_open() angewandt. */
static int apply_route(struct impl *this, const char *route)
{
	dm_config_port *dev;
	int res;

	if (!(dev = port_by_route_name(this, route))) {
		spa_log_warn(this->log, NAME " Route \"%s\" kennt der HAL nicht", route);
		return -ENOENT;
	}

	snprintf(this->wanted_port, sizeof(this->wanted_port), "%s", dev->name);

	if (!this->stream) {
		spa_log_info(this->log, NAME " Route \"%s\" gemerkt (HAL noch zu)", dev->name);
		return 0;
	}

	if (this->capture)
		res = pa_droid_hw_set_input_device(this->stream, dev) ? 0 : -EIO;
	else
		res = pa_droid_stream_set_route(this->stream, dev);

	if (res < 0)
		spa_log_warn(this->log, NAME " Route \"%s\" fehlgeschlagen: %d", dev->name, res);
	else
		DIAG(this, "Route gewechselt: %s -> %s", route, dev->name);
	return res;
}

/* Lautstaerke im Gespraech. Im Anruf fliesst kein PCM durch den Graphen - die
 * Software-Verstaerkung des Adapters greift also ins Leere. Der Pegel des
 * Sprachpfads sitzt im HAL und wird ueber set_voice_volume gesetzt; PulseAudios
 * droid-sink macht im Anrufprofil dasselbe. */
static int apply_voice_volume(struct impl *this, const char *value)
{
	float vol;

	if (this->capture || !this->hw)
		return 0;
	if (!this->in_call)
		return 0;   /* ausserhalb des Anrufs macht der HAL nichts damit */

	/* NICHT atof(): das liest den Punkt in einer Lokalisierung mit Komma als
	 * Dezimaltrennzeichen nicht - aus "0.343" wurde 0,00 und der Sprachpegel
	 * fiel auf null. spa_atof schaltet dafuer intern auf die C-Lokalisierung. */
	if (!spa_atof(value, &vol)) {
		spa_log_warn(this->log, NAME " Sprachlautstaerke \"%s\" nicht lesbar", value);
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
			spa_log_warn(this->log, NAME " Sprachlautstaerke %.2f abgelehnt (%d)", vol, r);
		else
			DIAG(this, "Sprachlautstaerke: %.2f", vol);
	} else {
		spa_log_warn(this->log, NAME " HAL bietet kein set_voice_volume");
	}
	pa_droid_hw_module_unlock(this->hw);
	return 0;
}

/* Anrufmodus. Der Modus gehoert dem HAL-Modul, nicht dem Stream - aber
 * pa_droid_hw_set_mode braucht den primaeren Ausgangsstream, um beim Wechsel
 * nach AUDIO_MODE_IN_CALL erst auf Lautsprecher und dann auf Ohrmuschel zu
 * routen (manche Geraete starten den Anruf sonst falsch). Deshalb wird der
 * HAL hier notfalls eigens geoeffnet - PulseAudio macht dasselbe mit einem
 * virtuellen Stream (voice_virtual_stream). */
static int apply_mode(struct impl *this, const char *mode)
{
	audio_mode_t m;
	int res;

	/* Nur der Wiedergabeknoten: er haelt den primaeren Ausgang. */
	if (this->capture)
		return 0;

	if (spa_streq(mode, "call"))
		m = AUDIO_MODE_IN_CALL;
	else if (spa_streq(mode, "communication"))
		m = AUDIO_MODE_IN_COMMUNICATION;
	else if (spa_streq(mode, "ringtone"))
		m = AUDIO_MODE_RINGTONE;
	else
		m = AUDIO_MODE_NORMAL;

	if (m != AUDIO_MODE_NORMAL) {
		if ((res = hal_open(this)) < 0) {
			spa_log_warn(this->log, NAME " Anrufmodus: HAL liess sich nicht oeffnen");
			return res;
		}
		this->mode_holds_hal = true;
	}

	if (!this->hw)
		return 0;   /* nichts offen, nichts zu tun */

	if (!pa_droid_hw_set_mode(this->hw, m)) {
		spa_log_warn(this->log, NAME " Audiomodus \"%s\" abgelehnt", mode);
		return -EIO;
	}
	this->in_call = m == AUDIO_MODE_IN_CALL;
	DIAG(this, "Audiomodus: %s", mode);

	/* Der HAL routet beim Eintritt in den Anruf selbst auf die Ohrmuschel.
	 * Die Karte weiss davon nichts und glaubt weiter an ihre Route - ein
	 * spaeteres Umschalten auf genau diese Route waere dann ein No-Op, und
	 * HAL und Karte blieben dauerhaft uneins. Deshalb die zuletzt vom Device
	 * gewuenschte Route neu setzen. */
	if (this->wanted_port[0] && this->stream) {
		dm_config_port *dev = dm_config_find_port(this->hw->enabled_module,
				this->wanted_port);
		if (dev)
			pa_droid_stream_set_route(this->stream, dev);
	}

	/* Anruf vorbei und nichts zu spielen: Hardware wieder freigeben. */
	if (m == AUDIO_MODE_NORMAL && this->mode_holds_hal) {
		this->mode_holds_hal = false;
		if (!this->started)
			hal_close(this);
	}
	return 0;
}

/* Vom Device gerufen: Route auf einen benannten devicePort umstellen. Ist der
 * HAL noch nicht offen, wird der Wunsch nur gemerkt und beim naechsten
 * hal_open() angewandt. */
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
