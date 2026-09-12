/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* The node, with the HAL replaced by something that counts bytes.
 *
 * See tests/hal-stub.c for why. In short: everything in droid-pcm.c that has
 * been wrong was a decision rather than a hardware detail - the ring buffer,
 * the give-up-after-three-failures rule, the latency arithmetic, the drain at
 * the end of playback, the audio source that a call takes away and never
 * gives back. Those are what is checked here.
 */
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <spa/support/log.h>
#include <spa/support/loop.h>
#include <spa/support/system.h>

#include "hal-stub.h"

/* Two things the node does that cannot fail on a desk but do fail on a phone:
 * an allocation that comes back empty and a thread that will not start. Both
 * are redirected here rather than interposed process-wide - the seam ends at
 * the #undef below, so the test's own code allocates normally. */
static void *test_malloc(size_t size);
static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
		void *(*fn)(void *), void *arg);
#define malloc(n) test_malloc(n)
#define pthread_create(a, b, c, d) test_pthread_create(a, b, c, d)
#include "../src/droid-pcm.c"
#undef malloc
#undef pthread_create

/* An allocation of exactly this many bytes comes back empty. Exact, so that
 * only the one the test aims at fails and printf keeps working. */
static size_t fail_malloc_size;
static bool fail_thread_create;

static void *test_malloc(size_t size)
{
	if (fail_malloc_size && size == fail_malloc_size)
		return NULL;
	return malloc(size);
}

static int test_pthread_create(pthread_t *thread, const pthread_attr_t *attr,
		void *(*fn)(void *), void *arg)
{
	if (fail_thread_create)
		return EAGAIN;
	return pthread_create(thread, attr, fn, arg);
}

/* ------------------------------------------------------------- harness */

static int failures;
static int checks;

static bool check(const char *what, bool cond)
{
	checks++;
	if (cond) {
		printf("  \033[32mok\033[0m   %s\n", what);
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m %s\n", what);
	}
	return cond;
}

static void check_int(const char *what, int64_t want, int64_t got)
{
	checks++;
	if (want == got) {
		printf("  \033[32mok\033[0m   %s\n", what);
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m %s: expected %lld, got %lld\n", what,
				(long long) want, (long long) got);
	}
}

static void check_str(const char *what, const char *want, const char *got)
{
	checks++;
	if (got && spa_streq(want, got)) {
		printf("  \033[32mok\033[0m   %s\n", what);
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m %s: expected \"%s\", got \"%s\"\n", what,
				want, got ? got : "(null)");
	}
}

static void section(const char *title)
{
	printf("\n%s\n", title);
}

/* Wait for the writer or reader thread to get somewhere. The stub answers
 * instantly, so this is a fraction of a millisecond in practice; the limit is
 * only there so a broken build fails instead of hanging. */
static bool wait_until(bool (*done)(void))
{
	int i;
	for (i = 0; i < 2000; i++) {
		if (done())
			return true;
		usleep(1000);
	}
	return false;
}

/* The other way round: something that must NOT happen. A thread that died on
 * an allocation never reports anything, so the proof is that nothing arrives
 * in a window in which a healthy thread would have done thousands. */
static bool never(bool (*happens)(void), int ms)
{
	int i;
	for (i = 0; i < ms; i++) {
		if (happens())
			return false;
		usleep(1000);
	}
	return true;
}

/* --------------------------------------------- a log that counts */

static struct {
	unsigned errors, warns;
	char last[512];
} logbook;

static void logbook_reset(void)
{
	memset(&logbook, 0, sizeof(logbook));
}

static void t_logv(void *object, enum spa_log_level level, const char *file,
		int line, const char *func, const char *fmt, va_list args)
{
	if (level == SPA_LOG_LEVEL_ERROR)
		logbook.errors++;
	else if (level == SPA_LOG_LEVEL_WARN)
		logbook.warns++;
	if (level <= SPA_LOG_LEVEL_WARN)
		vsnprintf(logbook.last, sizeof(logbook.last), fmt, args);
}

static void t_log(void *object, enum spa_log_level level, const char *file,
		int line, const char *func, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	t_logv(object, level, file, line, func, fmt, a);
	va_end(a);
}

static void t_logtv(void *object, enum spa_log_level level,
		const struct spa_log_topic *topic, const char *file, int line,
		const char *func, const char *fmt, va_list args)
{
	t_logv(object, level, file, line, func, fmt, args);
}

static void t_logt(void *object, enum spa_log_level level,
		const struct spa_log_topic *topic, const char *file, int line,
		const char *func, const char *fmt, ...)
{
	va_list a;
	va_start(a, fmt);
	t_logv(object, level, file, line, func, fmt, a);
	va_end(a);
}

static const struct spa_log_methods log_methods = {
	SPA_VERSION_LOG_METHODS,
	.log = t_log,
	.logv = t_logv,
	.logt = t_logt,
	.logtv = t_logtv,
};

static struct spa_log test_log = {
	.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Log, SPA_VERSION_LOG,
			&log_methods, NULL),
	.level = SPA_LOG_LEVEL_DEBUG,
};

/* ------------------------------------- system and loop, the real thing */

/* Timers are real timerfds: the clock arithmetic is worth testing against a
 * kernel that actually enforces it. Everything else the node does not use. */
static struct {
	unsigned settimes, closes, adds, removes, creates;
	struct itimerspec last;
	int timerfd_create_result;   /* < 0 to make it fail */
	int settime_result;          /* < 0 to make arming the timer fail */
} sys;

static int t_timerfd_create(void *object, int clockid, int flags)
{
	sys.creates++;
	if (sys.timerfd_create_result < 0)
		return sys.timerfd_create_result;
	return timerfd_create(clockid, TFD_CLOEXEC | TFD_NONBLOCK);
}

static int t_timerfd_settime(void *object, int fd, int flags,
		const struct itimerspec *nv, struct itimerspec *ov)
{
	sys.settimes++;
	sys.last = *nv;
	if (sys.settime_result < 0)
		return sys.settime_result;
	return timerfd_settime(fd, (flags & SPA_FD_TIMER_ABSTIME) ? TFD_TIMER_ABSTIME : 0,
			nv, ov);
}

static int t_timerfd_read(void *object, int fd, uint64_t *expirations)
{
	if (read(fd, expirations, sizeof(*expirations)) != sizeof(*expirations))
		return -errno;
	return 0;
}

static int t_clock_gettime(void *object, int clockid, struct timespec *value)
{
	return clock_gettime(clockid, value);
}

static int t_close(void *object, int fd)
{
	sys.closes++;
	return close(fd);
}

static const struct spa_system_methods system_methods = {
	SPA_VERSION_SYSTEM_METHODS,
	.close = t_close,
	.clock_gettime = t_clock_gettime,
	.timerfd_create = t_timerfd_create,
	.timerfd_settime = t_timerfd_settime,
	.timerfd_read = t_timerfd_read,
};

static struct spa_system test_system = {
	.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_System, SPA_VERSION_SYSTEM,
			&system_methods, NULL),
};

static int t_add_source(void *object, struct spa_source *source)
{
	sys.adds++;
	return 0;
}

static int t_remove_source(void *object, struct spa_source *source)
{
	sys.removes++;
	return 0;
}

static const struct spa_loop_methods loop_methods = {
	SPA_VERSION_LOOP_METHODS,
	.add_source = t_add_source,
	.remove_source = t_remove_source,
};

static struct spa_loop test_loop = {
	.iface = SPA_INTERFACE_INIT(SPA_TYPE_INTERFACE_Loop, SPA_VERSION_LOOP,
			&loop_methods, NULL),
};

static struct spa_support support[] = {
	{ SPA_TYPE_INTERFACE_Log, &test_log },
	{ SPA_TYPE_INTERFACE_DataLoop, &test_loop },
	{ SPA_TYPE_INTERFACE_DataSystem, &test_system },
};

/* ------------------------------------------------------------ fixtures */

static dm_config_device *fixture_config;
static dm_config_module *fixture_module;

/* Every test starts from the same place: an empty stub, an empty log, and a
 * HAL that hands out the fixture's module. That module is parsed separately
 * from the node's own copy on purpose - in the real thing the two are
 * different allocations, and code that confuses them has been a bug here. */
static void reset_all(void)
{
	hal_stub_reset();
	hal_stub.enabled_module = fixture_module;
	logbook_reset();
	memset(&sys, 0, sizeof(sys));
}

/* A node, built the way the daemon builds one. */
static struct impl *make_node(bool capture, const struct spa_dict *info)
{
	const struct spa_handle_factory *f = capture
		? &droid_pcm_source_factory : &droid_pcm_factory;
	struct spa_handle *h = calloc(1, f->get_size(f, NULL));
	int res;

	hw_module_keepalive = NULL;
	if ((res = f->init(f, h, info, support, SPA_N_ELEMENTS(support))) < 0) {
		free(h);
		errno = -res;
		return NULL;
	}
	return (struct impl *) h;
}

static void free_node(struct impl *this)
{
	if (!this)
		return;
	spa_handle_clear(&this->handle);
	free(this);
	hw_module_keepalive = NULL;
}

static const struct spa_dict *playback_info(void)
{
	static struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("droid.config", TEST_FIXTURE),
	};
	static struct spa_dict d;
	d = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));
	return &d;
}

/* Give the port a format, the way the adapter does. */
static void negotiate(struct impl *this, uint32_t rate, uint32_t channels)
{
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_audio_info_raw raw = {
		.format = SPA_AUDIO_FORMAT_S16_LE,
		.rate = rate,
		.channels = channels,
	};
	struct spa_pod *fmt = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);
	spa_node_port_set_param(&this->node, this->dir, 0, SPA_PARAM_Format, 0, fmt);
}

/* --------------------------------------------------------- what it is */

static void test_factories(void)
{
	const struct spa_interface_info *ii = NULL;
	uint32_t idx = 0;

	section("the two nodes the plugin offers");
	check_str("the sink is called api.droid.pcm",
			"api.droid.pcm", droid_pcm_factory.name);
	check_str("the source is called api.droid.pcm.source",
			"api.droid.pcm.source", droid_pcm_source_factory.name);
	check("both ask for room for their own state",
			droid_pcm_factory.get_size(&droid_pcm_factory, NULL) == sizeof(struct impl));
	check_int("a handle offers exactly one interface", 1,
			impl_enum_interface_info(&droid_pcm_factory, &ii, &idx));
	check("and that interface is a node",
			ii && spa_streq(ii->type, SPA_TYPE_INTERFACE_Node));
	check_int("there is no second one", 0,
			impl_enum_interface_info(&droid_pcm_factory, &ii, &idx));
}

static void test_init_defaults(void)
{
	struct impl *this;
	void *iface = NULL;

	section("a sink, started with nothing but a configuration file");
	this = make_node(false, playback_info());
	check("it comes up", this != NULL);
	if (!this)
		return;
	check("a sink takes data in, so its port is an input",
			this->dir == SPA_DIRECTION_INPUT);
	check_str("it uses the primary mix port unless told otherwise",
			"primary output", this->mix_port_name);
	check_str("the microphone source is set even on a sink, harmlessly",
			"mic", this->audio_source);
	check_str("the vendor option FuriOS uses is on by default",
			"speaker_before_voice=true", this->hw_options);
	check_int("48 kHz", 48000, this->pref_rate);
	check_int("in stereo", 2, this->pref_channels);
	check("the HAL module is loaded up front, before libhybris loses its "
			"address space", hw_module_keepalive != NULL);
	check_int("the node interface is handed out", 0,
			spa_handle_get_interface(&this->handle, SPA_TYPE_INTERFACE_Node, &iface));
	check("and it is the node", iface == &this->node);
	check_int("anything else is refused", -ENOENT,
			spa_handle_get_interface(&this->handle, "Spa:Pointer:Interface:Device", &iface));
	free_node(this);
}

static void test_init_capture(void)
{
	struct impl *this;

	section("a source, which is the same node the other way round");
	this = make_node(true, playback_info());
	check("it comes up", this != NULL);
	if (!this)
		return;
	check("a source hands data out, so its port is an output",
			this->dir == SPA_DIRECTION_OUTPUT);
	check_str("and it uses the primary input", "primary input", this->mix_port_name);
	check_int("it records from the built-in microphone",
			AUDIO_DEVICE_IN_BUILTIN_MIC, this->input_device);
	check("the port is live - the graph may not stall it",
			(this->port.info.flags & SPA_PORT_FLAG_LIVE) != 0);
	free_node(this);
}

static void test_init_properties(void)
{
	struct impl *this;
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("droid.config", TEST_FIXTURE),
		SPA_DICT_ITEM_INIT("droid.mix-port", "voip_rx"),
		SPA_DICT_ITEM_INIT("droid.device-port", "Wired Headset"),
		SPA_DICT_ITEM_INIT("droid.audio-source", "voice communication"),
		SPA_DICT_ITEM_INIT("droid.hw-options", "speaker_before_voice=false"),
		SPA_DICT_ITEM_INIT("audio.rate", "16000"),
		SPA_DICT_ITEM_INIT("audio.channels", "1"),
	};
	struct spa_dict d = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));

	section("the VoIP channel, which is the same node with other properties");
	this = make_node(false, &d);
	check("it comes up", this != NULL);
	if (!this)
		return;
	check_str("on the mix port it was given", "voip_rx", this->mix_port_name);
	check_str("with the device port it was given", "Wired Headset", this->input_port_name);
	check_str("and the audio source telephony needs",
			"voice communication", this->audio_source);
	check_str("and without the vendor option",
			"speaker_before_voice=false", this->hw_options);
	check_int("at 16 kHz, which is what the HAL insists on there", 16000, this->pref_rate);
	check_int("in mono", 1, this->pref_channels);
	free_node(this);
}

static void test_init_bad_properties(void)
{
	struct impl *this;
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("droid.config", TEST_FIXTURE),
		SPA_DICT_ITEM_INIT("audio.rate", "192000"),
		SPA_DICT_ITEM_INIT("audio.channels", "8"),
	};
	struct spa_dict d = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));

	section("nonsense in the properties");
	this = make_node(false, &d);
	if (!check("the node still comes up", this != NULL))
		return;
	check_int("a rate this HAL cannot do falls back to 48 kHz", 48000, this->pref_rate);
	check_int("and eight channels fall back to stereo", 2, this->pref_channels);
	free_node(this);
}

static void test_init_refusals(void)
{
	struct spa_handle *h;
	struct spa_support only_log[] = { { SPA_TYPE_INTERFACE_Log, &test_log } };
	struct spa_dict_item missing[] = {
		SPA_DICT_ITEM_INIT("droid.config", "/nonexistent/audio_policy.xml"),
	};
	struct spa_dict_item no_primary[] = {
		SPA_DICT_ITEM_INIT("droid.config", TEST_FIXTURE_NO_PRIMARY),
	};
	struct spa_dict d;

	section("when the node cannot come up");
	h = calloc(1, sizeof(struct impl));
	check_int("without a data loop it refuses rather than crashing later", -EINVAL,
			impl_init(&droid_pcm_factory, h, NULL, only_log,
				SPA_N_ELEMENTS(only_log)));
	free(h);

	h = calloc(1, sizeof(struct impl));
	d = SPA_DICT_INIT(missing, SPA_N_ELEMENTS(missing));
	check_int("an unreadable configuration is an error, not a guess", -EIO,
			impl_init(&droid_pcm_factory, h, &d, support, SPA_N_ELEMENTS(support)));
	free(h);

	h = calloc(1, sizeof(struct impl));
	d = SPA_DICT_INIT(no_primary, SPA_N_ELEMENTS(no_primary));
	check_int("and a file without the primary module too", -ENOENT,
			impl_init(&droid_pcm_factory, h, &d, support, SPA_N_ELEMENTS(support)));
	free(h);
	hw_module_keepalive = NULL;
}

/* ------------------------------------------------- opening the hardware */

static void test_hal_open_playback(void)
{
	struct impl *this;

	section("opening the HAL for playback");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);

	check_int("the stream opens", 0, hal_open(this));
	check_int("one output stream, no more", 1, hal_stub.output_opens);
	check_str("routed to the default output of the configuration",
			"Speaker", hal_stub.last_route);
	check_int("and routed exactly once", 1, hal_stub.route_calls);
	check("the HAL volume is pinned to full scale - the level is applied in "
			"the graph", hal_stub.volume_left == 1.0f && hal_stub.volume_right == 1.0f);
	check_int("the latency the HAL reports is remembered",
			42u * 1000 * 1000, (int64_t) this->hal_latency_ns);
	check_int("opening again does nothing", 0, hal_open(this));
	check_int("still one stream", 1, hal_stub.output_opens);

	hal_close(this);
	check("closing lets the stream go", this->stream == NULL);
	check("and the module too", this->hw == NULL);
	check_int("and the reported latency goes back to zero", 0,
			(int64_t) this->hal_latency_ns);
	free_node(this);
}

static void test_hal_open_playback_variants(void)
{
	struct impl *this;

	section("playback, when the HAL is awkward");
	reset_all();
	hal_stub.is_primary = false;
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	check_int("a secondary stream opens", 0, hal_open(this));
	check_int("but does not route - the primary stream does that for all",
			0, hal_stub.route_calls);
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.no_set_volume = true;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("a HAL without set_volume still opens", 0, hal_open(this));
	check_int("it just says so", 1, logbook.warns);
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.rate = 44100;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("a HAL that takes another rate still opens", 0, hal_open(this));
	check("but warns that it will sound out of tune",
			strstr(logbook.last, "out of tune") != NULL);
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.set_route_result = -1;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("routing that fails does not stop the stream", 0, hal_open(this));
	check("it is only reported", logbook.warns == 1);
	hal_close(this);
	free_node(this);
}

static void test_hal_open_failures(void)
{
	struct impl *this;

	section("when the hardware is not available");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	hal_stub.module_works = false;
	check_int("no HAL module, no stream", -EIO, hal_open(this));
	check("and nothing left half-open", this->hw == NULL);
	free_node(this);

	reset_all();
	hal_stub.open_output_works = false;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("a stream the HAL refuses", -EIO, hal_open(this));
	check("gives the module back", this->hw == NULL);
	free_node(this);

	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	snprintf(this->mix_port_name, sizeof(this->mix_port_name), "no such port");
	check_int("a mix port that is not in the configuration", -ENOENT, hal_open(this));
	check("gives the module back too", this->hw == NULL);
	free_node(this);
}

static void test_hal_open_capture(void)
{
	struct impl *this;

	section("opening the HAL for capture");
	reset_all();
	this = make_node(true, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);

	check_int("the stream opens", 0, hal_open(this));
	check_int("one input stream", 1, hal_stub.input_opens);
	check_str("and the audio source is set - the HAL ties its microphone "
			"processing to it", "mic", hal_stub.last_audio_source);
	check_str("routed to the built-in microphone",
			"Built-In Mic", hal_stub.last_input_device);
	/* 4096 B at 48 kHz stereo 16 bit = 4096 / 192000 s */
	check_int("input latency is estimated from the buffer, because the HAL "
			"reports none", 4096ll * SPA_NSEC_PER_SEC / 192000,
			(int64_t) this->hal_latency_ns);
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.open_input_works = false;
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	check_int("an input the HAL refuses", -EIO, hal_open(this));
	check("gives the module back", this->hw == NULL);
	free_node(this);

	reset_all();
	hal_stub.rate = 44100;
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	check_int("an input at the wrong rate is refused rather than bent",
			-EINVAL, hal_open(this));
	check("the stream is handed back", hal_stub.stream_unrefs == 1);
	free_node(this);

	reset_all();
	hal_stub.reconfigure_works = false;
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	check_int("an audio source the HAL will not take is not fatal",
			0, hal_open(this));
	check("it is reported", logbook.warns >= 1);
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.set_input_device_works = false;
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	check_int("nor is routing the microphone that fails", 0, hal_open(this));
	check("also reported", logbook.warns >= 1);
	hal_close(this);
	free_node(this);
}

static void test_hal_open_capture_ports(void)
{
	struct impl *this;
	struct spa_dict_item items[] = {
		SPA_DICT_ITEM_INIT("droid.config", TEST_FIXTURE),
		SPA_DICT_ITEM_INIT("droid.device-port", "Built-In Back Mic"),
	};
	struct spa_dict d = SPA_DICT_INIT(items, SPA_N_ELEMENTS(items));

	section("which microphone the capture node opens");
	reset_all();
	this = make_node(true, &d);
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	check_int("it opens", 0, hal_open(this));
	check_str("on the device port from its properties",
			"Built-In Back Mic", hal_stub.last_input_device);
	hal_close(this);
	free_node(this);

	reset_all();
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	snprintf(this->wanted_port, sizeof(this->wanted_port), "Voice Call In");
	check_int("it opens", 0, hal_open(this));
	check_str("a route asked for earlier wins over the property",
			"Voice Call In", hal_stub.last_input_device);
	hal_close(this);
	free_node(this);

	reset_all();
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	this->input_device = 0xdead;
	check_int("it opens", 0, hal_open(this));
	check("a device type that is not in the configuration leaves the HAL "
			"routing alone", hal_stub.input_device_calls == 0);
	hal_close(this);
	free_node(this);
}

/* ------------------------------------------------------------- routing */

static const char *route_port(struct impl *this, const char *route)
{
	dm_config_port *p = port_by_route_name(this, route);
	return p ? p->name : NULL;
}

static void test_route_names(void)
{
	struct impl *this;

	section("translating route names into HAL ports");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	check_str("output-speaker", "Speaker", route_port(this, "output-speaker"));
	check_str("output-earpiece", "Earpiece", route_port(this, "output-earpiece"));
	check_str("output-wired_headset", "Wired Headset",
			route_port(this, "output-wired_headset"));
	check_str("input-builtin_mic", "Built-In Mic",
			route_port(this, "input-builtin_mic"));
	check_str("input-back_mic", "Built-In Back Mic",
			route_port(this, "input-back_mic"));
	check_str("input-voice_call", "Voice Call In",
			route_port(this, "input-voice_call"));
	check("a route the configuration does not have is not invented",
			route_port(this, "output-carrier_pigeon") == NULL);

	/* The device's audio_policy XML has Bluetooth commented out, so the two
	 * SCO ports are built here rather than looked up. */
	{
		dm_config_port *p = port_by_route_name(this, "output-bluetooth_sco");
		check("Bluetooth output is supplied by us, not by the file",
				p != NULL && p->type == AUDIO_DEVICE_OUT_BLUETOOTH_SCO);
		check("as a sink", p && p->role == DM_CONFIG_ROLE_SINK);
		p = port_by_route_name(this, "input-bluetooth_sco_headset");
		check("and so is the headset microphone",
				p != NULL && p->type == AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET);
		check("as a source", p && p->role == DM_CONFIG_ROLE_SOURCE);
	}
	free_node(this);
}

static void test_apply_route(void)
{
	struct impl *this;

	section("changing route while the HAL is closed and while it is open");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	check_int("a route is accepted before anything is open", 0,
			apply_route(this, "output-earpiece"));
	check_str("and remembered", "Earpiece", this->wanted_port);
	check_int("nothing was sent to the HAL", 0, hal_stub.route_calls);
	check_int("an unknown route is refused", -ENOENT,
			apply_route(this, "output-nowhere"));

	negotiate(this, 48000, 2);
	check_int("the stream opens", 0, hal_open(this));
	check_str("on the remembered route rather than the default",
			"Earpiece", hal_stub.last_route);
	check_int("switching now reaches the HAL", 0,
			apply_route(this, "output-speaker"));
	check_str("which route it is", "Speaker", hal_stub.last_route);

	hal_stub.set_route_result = -EIO;
	check_int("a route the HAL rejects is reported", -EIO,
			apply_route(this, "output-earpiece"));
	hal_stub.set_route_result = 0;

	hal_stub.is_primary = false;
	hal_stub.route_calls = 0;
	check_int("a secondary stream accepts the route", 0,
			apply_route(this, "output-speaker"));
	check_int("but leaves the routing to the primary one", 0, hal_stub.route_calls);
	hal_close(this);
	free_node(this);

	reset_all();
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	hal_open(this);
	check_int("a capture node routes its microphone", 0,
			apply_route(this, "input-back_mic"));
	check_str("to the port asked for", "Built-In Back Mic", hal_stub.last_input_device);
	hal_stub.set_input_device_works = false;
	check_int("and reports it when the HAL says no", -EIO,
			apply_route(this, "input-builtin_mic"));
	hal_close(this);
	free_node(this);
}

/* A route change that crosses Bluetooth has to reopen the stream.
 *
 * The HAL picks the hardware path while OPENING, not when a route is set on
 * an open stream. Measured on the phone: the route set on a running stream
 * left the HAL on pcmC0D0p and the sound came out of the speaker while every
 * log line said "BT SCO"; set before opening, the HAL went to pcmC0D55p, the
 * Bluetooth PCM device. So the crossing is a close and an open, and only the
 * crossing - speaker, earpiece and the wired accessories share one PCM device
 * and reroute fine on an open stream, where a reopen would only cost a gap.
 */
static void test_bluetooth_reopen(void)
{
	struct impl *this;
	unsigned opens, unrefs;

	section("a route that crosses Bluetooth reopens the stream");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	check_int("the stream opens", 0, hal_open(this));
	opens = hal_stub.output_opens;
	unrefs = hal_stub.stream_unrefs;

	/* Within the phone's own outputs nothing is torn down. */
	hal_stub.route_calls = 0;
	check_int("earpiece is accepted", 0, apply_route(this, "output-earpiece"));
	check_int("without reopening anything", opens, hal_stub.output_opens);
	check_int("the open stream is simply rerouted", 1, hal_stub.route_calls);
	check_str("to the earpiece", "Earpiece", hal_stub.last_route);

	/* Onto the headset: this one has to go through a reopen. */
	check_int("Bluetooth is accepted", 0,
			apply_route(this, "output-bluetooth_sco"));
	check_int("the old stream is closed", unrefs + 1, hal_stub.stream_unrefs);
	check_int("and a new one opened", opens + 1, hal_stub.output_opens);
	check_str("on the Bluetooth port", "BT SCO", hal_stub.last_route);
	check("with BT_SCO=on before the open, which is what routes the HAL",
			strstr(hal_stub.last_parameters, "BT_SCO=on") != NULL);

	/* And back off it again - the crossing counts in both directions. */
	opens = hal_stub.output_opens;
	check_int("leaving Bluetooth is accepted", 0,
			apply_route(this, "output-speaker"));
	check_int("and reopens as well", opens + 1, hal_stub.output_opens);
	check_str("back on the speaker", "Speaker", hal_stub.last_route);

	/* Two Bluetooth ports in a row do not cross anything. */
	apply_route(this, "output-bluetooth_sco");
	opens = hal_stub.output_opens;
	hal_stub.route_calls = 0;
	check_int("staying on Bluetooth is accepted", 0,
			apply_route(this, "output-bluetooth_sco"));
	check_int("and does not reopen", opens, hal_stub.output_opens);
	check_int("it only reroutes", 1, hal_stub.route_calls);

	hal_close(this);
	free_node(this);

	/* The failure that matters: the new route cannot be opened. The node
	 * must not be left without a stream - a phone that stays silent until
	 * the next reboot is worse than a headset that did not take over. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	hal_open(this);

	hal_stub.output_opens_failing = 1;
	check_int("a crossing that cannot be opened is reported", -EIO,
			apply_route(this, "output-bluetooth_sco"));
	check("but the node still has a stream", this->stream != NULL);
	check_str("on the route that was working", "Speaker", hal_stub.last_route);
	check_str("and that is the route it remembers", "output-speaker",
			this->wanted_route);

	hal_close(this);
	free_node(this);
}

/* The codec on the Bluetooth link.
 *
 * bt_wbs is a module parameter and the HAL reads it when the stream is
 * opened, so it has to be right beforehand - and changing it under an open
 * Bluetooth stream means reopening, exactly as crossing into Bluetooth does.
 * Getting it wrong is silence that measures perfectly: on the phone the link
 * stood, BTCVSD Tx Irq was on, the HAL held pcmC0D55p, and the earbuds - busy
 * decoding CVSD bytes as mSBC - played nothing at all.
 */
static void test_bt_codec(void)
{
	struct impl *this;
	unsigned opens;

	section("the codec the Bluetooth link was negotiated with");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	check_int("a codec is accepted before anything is open", 0,
			apply_bt_wbs(this, "on"));
	check_int("nonsense is refused", -EINVAL, apply_bt_wbs(this, "sometimes"));
	check_str("and the refusal changes nothing", "on", this->bt_wbs);

	negotiate(this, 48000, 2);
	apply_route(this, "output-bluetooth_sco");
	hal_stub.all_parameters[0] = '\0';
	check_int("the stream opens", 0, hal_open(this));
	check("the codec reached the HAL", strstr(hal_stub.all_parameters, "bt_wbs=on") != NULL);
	check("and BT_SCO=on did too", strstr(hal_stub.all_parameters, "BT_SCO=on") != NULL);
	/* The order matters: the HAL reads both while opening, and a codec that
	 * arrives after BT_SCO=on is a codec the stream did not get. */
	check("the codec came first",
			strstr(hal_stub.all_parameters, "bt_wbs=on") <
			strstr(hal_stub.all_parameters, "BT_SCO=on"));

	/* Changing it under an open Bluetooth stream has to reopen: the HAL read
	 * the old value when it opened. */
	opens = hal_stub.output_opens;
	check_int("switching to narrow-band is accepted", 0, apply_bt_wbs(this, "off"));
	check_int("and reopens the stream", opens + 1, hal_stub.output_opens);

	/* The same value twice is not a change. */
	opens = hal_stub.output_opens;
	check_int("the same codec again is accepted", 0, apply_bt_wbs(this, "off"));
	check_int("and does not reopen", opens, hal_stub.output_opens);

	hal_close(this);
	free_node(this);

	/* Away from Bluetooth the codec is only remembered - there is nothing to
	 * reopen for, and tearing down a speaker stream would be a gap in the
	 * audio for nothing. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	hal_open(this);
	opens = hal_stub.output_opens;
	check_int("a codec change off Bluetooth is accepted", 0,
			apply_bt_wbs(this, "on"));
	check_int("and reopens nothing", opens, hal_stub.output_opens);
	check_str("but is remembered for the next open", "on", this->bt_wbs);
	hal_close(this);
	free_node(this);

	/* And when nobody ever said which codec: the HAL stays on its narrow-band
	 * default, which is a working link that the headset cannot decode. Open
	 * it, but do not pretend it is fine. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-bluetooth_sco");
	hal_stub.all_parameters[0] = '\0';
	check_int("Bluetooth opens even with no codec named", 0, hal_open(this));
	check("and nothing about a codec was invented",
			strstr(hal_stub.all_parameters, "bt_wbs") == NULL);
	hal_close(this);
	free_node(this);
}

static void test_registry(void)
{
	struct impl *sink, *source;

	section("finding the right node from the device, across the process");
	reset_all();
	sink = make_node(false, playback_info());
	source = make_node(true, playback_info());
	if (!check("both nodes are there", sink && source))
		return;

	check_int("a route for the primary output arrives at the sink", 0,
			droid_node_set_route("primary output", "output-earpiece"));
	check_str("and is remembered there", "Earpiece", sink->wanted_port);
	check_int("a route for the primary input arrives at the source", 0,
			droid_node_set_route("primary input", "input-back_mic"));
	check_str("and there", "Built-In Back Mic", source->wanted_port);
	check_int("a mix port nobody registered is refused", -ENOENT,
			droid_node_set_route("voip_rx", "output-speaker"));
	check_int("an unknown route is refused as well", -ENOENT,
			droid_node_set_route("primary output", "output-nowhere"));

	free_node(source);
	check_int("a node that is gone is no longer reachable", -ENOENT,
			droid_node_set_route("primary input", "input-builtin_mic"));
	free_node(sink);
}

/* ------------------------------------------------------- call handling */

static void test_apply_mode(void)
{
	struct impl *this;

	section("going into a call and back out again");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);

	check_int("the call mode is accepted", 0, apply_mode(this, "call"));
	check_int("the HAL is told", AUDIO_MODE_IN_CALL, hal_stub.mode);
	check("and it was opened for it, even though nothing is playing",
			this->stream != NULL);
	check("the node knows the HAL is only open for the call",
			this->mode_holds_hal);
	check("and that it is in one", this->in_call);

	check_int("ringtone mode", 0, apply_mode(this, "ringtone"));
	check_int("reaches the HAL", AUDIO_MODE_RINGTONE, hal_stub.mode);
	check("but is not a call", !this->in_call);
	check_int("VoIP mode", 0, apply_mode(this, "communication"));
	check_int("reaches the HAL too", AUDIO_MODE_IN_COMMUNICATION, hal_stub.mode);
	check_int("anything else is normal", 0, apply_mode(this, "whatever"));
	check_int("which is what the HAL gets", AUDIO_MODE_NORMAL, hal_stub.mode);
	check("nothing plays, so the hardware is released", this->stream == NULL);
	check("and the node no longer holds it for a call", !this->mode_holds_hal);
	free_node(this);
}

static void test_apply_mode_details(void)
{
	struct impl *this;

	section("the two things a call breaks if nobody sees to them");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	hal_open(this);
	hal_stub.route_calls = 0;
	apply_mode(this, "call");
	check_int("entering a call re-applies the route, because the HAL moves to "
			"the earpiece on its own", 1, hal_stub.route_calls);
	check_str("to the route the card believes in", "Speaker", hal_stub.last_route);
	apply_mode(this, "normal");
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.realcall = true;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_mode(this, "call");
	check_str("with the realcall option the DSP is told to open the voice path",
			"realcall=on", hal_stub.last_parameters);
	apply_mode(this, "normal");
	check_str("and to close it again", "realcall=off", hal_stub.last_parameters);
	free_node(this);

	reset_all();
	hal_stub.realcall = true;
	hal_stub.set_parameters_result = -EINVAL;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_mode(this, "call");
	check("a HAL that rejects realcall - as this MediaTek one does - is only "
			"reported", logbook.warns >= 1);
	apply_mode(this, "normal");
	free_node(this);

	reset_all();
	hal_stub.set_mode_works = false;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("a mode the HAL refuses is an error", -EIO, apply_mode(this, "call"));
	free_node(this);

	reset_all();
	hal_stub.module_works = false;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("no HAL, no call", -EIO, apply_mode(this, "call"));
	free_node(this);

	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("normal mode with nothing open does nothing at all", 0,
			apply_mode(this, "normal"));
	check_int("and says nothing to the HAL", 0, hal_stub.mode_calls);
	free_node(this);
}

static void test_audio_source_after_call(void)
{
	struct impl *this;

	section("the microphone that a call takes away");
	reset_all();
	this = make_node(true, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	hal_open(this);
	hal_stub.reconfigure_calls = 0;
	hal_stub.last_audio_source[0] = '\0';

	check_int("a capture node does not set the mode - the sink owns it", 0,
			apply_mode(this, "call"));
	check_int("so the HAL hears nothing about it", 0, hal_stub.mode_calls);
	check_int("and nothing is reconfigured yet", 0, hal_stub.reconfigure_calls);

	check_int("when the call ends", 0, apply_mode(this, "normal"));
	check_int("the audio source is set again", 1, hal_stub.reconfigure_calls);
	check_str("to the one the node was configured with - without this every "
			"recording after a call is digital silence",
			"mic", hal_stub.last_audio_source);

	hal_stub.reconfigure_works = false;
	apply_mode(this, "normal");
	check("a HAL that will not take it back says so", logbook.warns >= 1);
	hal_close(this);
	free_node(this);

	reset_all();
	this = make_node(true, playback_info());
	this->audio_source[0] = '\0';
	negotiate(this, 48000, 2);
	hal_open(this);
	hal_stub.reconfigure_calls = 0;
	apply_mode(this, "normal");
	check_int("a node without an audio source has nothing to restore", 0,
			hal_stub.reconfigure_calls);
	hal_close(this);
	free_node(this);
}

static void test_voice_volume(void)
{
	struct impl *this;

	section("the volume during a call, which lives in the HAL");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);

	check_int("with no HAL open there is nothing to set", 0,
			apply_voice_volume(this, "0.5"));
	hal_open(this);
	check_int("outside a call the HAL does nothing with it", 0,
			apply_voice_volume(this, "0.5"));
	check_int("so it is not even sent", 0, hal_stub.voice_volume_calls);

	apply_mode(this, "call");
	check_int("in a call it arrives", 0, apply_voice_volume(this, "0.343"));
	check("with the decimal point read as a decimal point - a comma locale "
			"once turned this into 0.00",
			fabsf(hal_stub.voice_volume - 0.343f) < 0.0001f);
	check_int("more than full scale is clamped", 0, apply_voice_volume(this, "2.0"));
	check("to 1.0", hal_stub.voice_volume == 1.0f);
	check_int("less than nothing is clamped too", 0, apply_voice_volume(this, "-1"));
	check("to 0.0", hal_stub.voice_volume == 0.0f);
	check_int("something that is not a number is refused", -EINVAL,
			apply_voice_volume(this, "loud"));

	hal_stub.set_voice_volume_result = -EIO;
	check_int("a level the HAL rejects is reported", 0,
			apply_voice_volume(this, "0.5"));
	check("as a warning", logbook.warns >= 1);
	apply_mode(this, "normal");
	free_node(this);

	reset_all();
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	hal_open(this);
	check_int("a capture node has no voice volume to set", 0,
			apply_voice_volume(this, "0.5"));
	check_int("and sets none", 0, hal_stub.voice_volume_calls);
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.no_voice_volume = true;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_mode(this, "call");
	check_int("a HAL without set_voice_volume is not an error", 0,
			apply_voice_volume(this, "0.5"));
	check("it is reported once", logbook.warns >= 1);
	apply_mode(this, "normal");
	free_node(this);
}

/* ------------------------------------------------- format and parameters */

/* Collect what the node emits. */
static struct {
	unsigned infos, port_infos, results;
	uint32_t last_param_id;
	struct spa_pod *last_param;
	uint8_t store[2048];
} emitted;

static void on_info(void *data, const struct spa_node_info *info)
{
	emitted.infos++;
}

static void on_port_info(void *data, enum spa_direction dir, uint32_t id,
		const struct spa_port_info *info)
{
	emitted.port_infos++;
}

static void on_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	const struct spa_result_node_params *r = result;
	emitted.results++;
	emitted.last_param_id = r->id;
	if (r->param && SPA_POD_SIZE(r->param) <= sizeof(emitted.store)) {
		memcpy(emitted.store, r->param, SPA_POD_SIZE(r->param));
		emitted.last_param = (struct spa_pod *) emitted.store;
	}
}

static const struct spa_node_events node_events = {
	SPA_VERSION_NODE_EVENTS,
	.info = on_info,
	.port_info = on_port_info,
	.result = on_result,
};

static int enum_params(struct impl *this, uint32_t id, const struct spa_pod *filter)
{
	memset(&emitted, 0, sizeof(emitted));
	return spa_node_port_enum_params(&this->node, 0, this->dir, 0, id, 0, 1, filter);
}

static void test_listener_and_params(void)
{
	struct impl *this;
	struct spa_hook listener = { 0 };
	uint8_t buf[512];
	struct spa_pod_builder b;

	section("what the node tells the graph about itself");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	memset(&emitted, 0, sizeof(emitted));
	spa_node_add_listener(&this->node, &listener, &node_events, NULL);
	check_int("a new listener is told about the node", 1, emitted.infos);
	check_int("and about its port", 1, emitted.port_infos);
	spa_hook_remove(&listener);
	spa_node_add_listener(&this->node, &listener, &node_events, NULL);

	check_int("it offers one format", 0, enum_params(this, SPA_PARAM_EnumFormat, NULL));
	check_int("and emits it", 1, emitted.results);
	check_int("asking for the current format without one is an error", -EIO,
			enum_params(this, SPA_PARAM_Format, NULL));
	check_int("so is asking how big the buffers should be", -EIO,
			enum_params(this, SPA_PARAM_Buffers, NULL));
	check_int("the IO area is described", 0, enum_params(this, SPA_PARAM_IO, NULL));
	check_int("and the latency", 0, enum_params(this, SPA_PARAM_Latency, NULL));
	check_int("a parameter the node does not have is refused", -ENOENT,
			enum_params(this, SPA_PARAM_Profile, NULL));
	check_int("and so is the wrong direction", -EINVAL,
			spa_node_port_enum_params(&this->node, 0, SPA_DIRECTION_OUTPUT, 0,
				SPA_PARAM_EnumFormat, 0, 1, NULL));

	negotiate(this, 48000, 2);
	check_int("with a format it reports it", 0, enum_params(this, SPA_PARAM_Format, NULL));
	check_int("once", 1, emitted.results);
	check_int("and only once", 0,
			spa_node_port_enum_params(&this->node, 0, this->dir, 0,
				SPA_PARAM_Format, 1, 1, NULL));
	check_int("and now it can size buffers", 0, enum_params(this, SPA_PARAM_Buffers, NULL));
	check_int("also only once", 0,
			spa_node_port_enum_params(&this->node, 0, this->dir, 0,
				SPA_PARAM_Buffers, 1, 1, NULL));
	check_int("the IO area only once as well", 0,
			spa_node_port_enum_params(&this->node, 0, this->dir, 0,
				SPA_PARAM_IO, 1, 1, NULL));
	check_int("and the latency only once", 0,
			spa_node_port_enum_params(&this->node, 0, this->dir, 0,
				SPA_PARAM_Latency, 1, 1, NULL));

	/* A filter that cannot match: the node has to keep looking and then run
	 * out of formats rather than emit something the filter excludes. */
	spa_pod_builder_init(&b, buf, sizeof(buf));
	{
		struct spa_pod *filter = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
			SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video));
		check_int("a filter nothing matches yields nothing", 0,
				enum_params(this, SPA_PARAM_EnumFormat, filter));
		check_int("and emits nothing", 0, emitted.results);
	}

	spa_hook_remove(&listener);
	free_node(this);
}

static void test_buffer_size(void)
{
	struct impl *this;
	struct spa_hook listener = { 0 };
	const struct spa_pod_prop *p;
	int32_t size = 0;

	section("how big a buffer the node asks for");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	spa_node_add_listener(&this->node, &listener, &node_events, NULL);
	negotiate(this, 48000, 2);

	enum_params(this, SPA_PARAM_Buffers, NULL);
	p = spa_pod_find_prop(emitted.last_param, NULL, SPA_PARAM_BUFFERS_size);
	if (p)
		spa_pod_get_int(&p->value, &size);
	check_int("with no stream open it falls back to a HAL period", 4096, size);

	negotiate(this, 48000, 2);
	this->quantum = 2048;
	enum_params(this, SPA_PARAM_Buffers, NULL);
	p = spa_pod_find_prop(emitted.last_param, NULL, SPA_PARAM_BUFFERS_size);
	if (p)
		spa_pod_get_int(&p->value, &size);
	check_int("a larger graph quantum wins - the HAL period is not the graph's",
			2048 * 4, size);

	spa_hook_remove(&listener);
	free_node(this);
}

static void test_set_format(void)
{
	struct impl *this;
	uint8_t buf[512];
	struct spa_pod_builder b;
	struct spa_audio_info_raw raw;

	section("agreeing on a format");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	negotiate(this, 48000, 2);
	check("a format the HAL can do is taken", this->port.have_format);
	check_int("at the rate asked for", 48000,
			this->port.current_format.info.raw.rate);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	raw = (struct spa_audio_info_raw) {
		.format = SPA_AUDIO_FORMAT_F32_LE, .rate = 48000, .channels = 2 };
	check_int("floating point is refused - the HAL takes 16 bit", -EINVAL,
			spa_node_port_set_param(&this->node, this->dir, 0, SPA_PARAM_Format, 0,
				spa_format_audio_raw_build(&b, SPA_PARAM_Format, &raw)));

	spa_pod_builder_init(&b, buf, sizeof(buf));
	{
		struct spa_pod *not_audio = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_video),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw));
		check_int("and so is video", -EINVAL,
				spa_node_port_set_param(&this->node, this->dir, 0,
					SPA_PARAM_Format, 0, not_audio));
	}

	spa_pod_builder_init(&b, buf, sizeof(buf));
	{
		struct spa_pod *encoded = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_mp3));
		check_int("compressed audio too", -EINVAL,
				spa_node_port_set_param(&this->node, this->dir, 0,
					SPA_PARAM_Format, 0, encoded));
	}

	spa_pod_builder_init(&b, buf, sizeof(buf));
	{
		struct spa_pod *incomplete = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType,    SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw));
		check_int("raw audio without the details is refused", -EINVAL,
				spa_node_port_set_param(&this->node, this->dir, 0,
					SPA_PARAM_Format, 0, incomplete));
	}

	check("something that is not a format at all is refused",
			spa_node_port_set_param(&this->node, this->dir, 0, SPA_PARAM_Format, 0,
				(const struct spa_pod *) &SPA_POD_INIT_Int(5)) < 0);

	check_int("no format means the port is going away", 0,
			spa_node_port_set_param(&this->node, this->dir, 0,
				SPA_PARAM_Format, 0, NULL));
	check("and the node forgets it", !this->port.have_format);
	check_int("a parameter that is not the format is refused", -ENOENT,
			spa_node_port_set_param(&this->node, this->dir, 0,
				SPA_PARAM_Props, 0, NULL));
	free_node(this);
}

static void test_io_areas(void)
{
	struct impl *this;
	struct spa_io_clock clock = { 0 };
	struct spa_io_position pos = { 0 };
	struct spa_io_buffers io = { 0 };

	section("the shared areas the graph hands over");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	check_int("the clock is taken", 0,
			spa_node_set_io(&this->node, SPA_IO_Clock, &clock, sizeof(clock)));
	check("and remembered", this->clock == &clock);
	check_int("a clock that is too small is ignored rather than trusted", 0,
			spa_node_set_io(&this->node, SPA_IO_Clock, &clock, 4));
	check("so nothing points at it", this->clock == NULL);
	check_int("the position is taken", 0,
			spa_node_set_io(&this->node, SPA_IO_Position, &pos, sizeof(pos)));
	check("and remembered", this->position == &pos);
	check_int("a position that is too small is ignored", 0,
			spa_node_set_io(&this->node, SPA_IO_Position, &pos, 4));
	check("so nothing points at that either", this->position == NULL);
	check_int("anything else is refused", -ENOENT,
			spa_node_set_io(&this->node, SPA_IO_Control, &pos, sizeof(pos)));

	check_int("the port takes its buffer area", 0,
			spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers,
				&io, sizeof(io)));
	check("and remembers it", this->port.io == &io);
	check_int("and refuses anything else", -ENOENT,
			spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Control,
				&io, sizeof(io)));
	free_node(this);
}

/* ------------------------------------------------------- the data path */

struct testbuf {
	struct spa_buffer buf;
	struct spa_data data;
	struct spa_chunk chunk;
	uint8_t mem[8192];
};

static void testbuf_init(struct testbuf *t, uint32_t maxsize)
{
	memset(t, 0, sizeof(*t));
	t->data.type = SPA_DATA_MemPtr;
	t->data.maxsize = maxsize;
	t->data.data = t->mem;
	t->data.chunk = &t->chunk;
	t->buf.n_datas = 1;
	t->buf.datas = &t->data;
}

static void test_use_buffers(void)
{
	struct impl *this;
	static struct testbuf t[40];
	struct spa_buffer *ptrs[40];
	unsigned i;

	section("the buffers the graph offers");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	for (i = 0; i < SPA_N_ELEMENTS(t); i++) {
		testbuf_init(&t[i], 4096);
		ptrs[i] = &t[i].buf;
	}
	check_int("a handful of buffers are taken", 0,
			spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 4));
	check_int("all four", 4, this->port.n_buffers);
	check_int("more than the node has room for is taken as far as it goes", 0,
			spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 40));
	check_int("up to the limit", 32, this->port.n_buffers);
	free_node(this);
}

static void test_playback_ring(void)
{
	struct impl *this;
	static struct testbuf t;
	struct spa_buffer *ptrs[1];
	struct spa_io_buffers io = { 0 };
	unsigned i;

	section("filling the ring buffer, without a thread to empty it");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	testbuf_init(&t, 4096);
	t.chunk.size = 4096;
	ptrs[0] = &t.buf;
	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 1);

	check_int("without a buffer area there is nowhere to read from", -EIO,
			spa_node_process(&this->node));
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &io, sizeof(io));

	io.status = SPA_STATUS_OK;
	check_int("nothing to do when the graph has no data", SPA_STATUS_OK,
			spa_node_process(&this->node));

	io.status = SPA_STATUS_HAVE_DATA;
	io.buffer_id = 7;
	check_int("a buffer that was never handed over is refused", -EINVAL,
			spa_node_process(&this->node));

	io.buffer_id = 0;
	check_int("a full buffer is taken and more asked for", SPA_STATUS_NEED_DATA,
			spa_node_process(&this->node));
	check_int("4 kB in the ring", 4096, (int64_t) this->bytes_queued);

	/* 256 kB of ring, 4 kB at a time: the 64th call fills it. */
	for (i = 1; i < RING_SIZE / 4096; i++) {
		io.status = SPA_STATUS_HAVE_DATA;
		spa_node_process(&this->node);
	}
	check_int("the ring holds exactly what it says", RING_SIZE,
			(int64_t) this->bytes_queued);
	check_int("and nothing was dropped on the way", 0, this->n_overrun);
	io.status = SPA_STATUS_HAVE_DATA;
	spa_node_process(&this->node);
	check_int("one more block is dropped rather than overwriting the ring",
			1, this->n_overrun);
	check_int("and does not count as queued", RING_SIZE, (int64_t) this->bytes_queued);
	io.status = SPA_STATUS_HAVE_DATA;
	spa_node_process(&this->node);
	check_int("further losses are only counted, not logged again", 2, this->n_overrun);
	check_int("with one warning in total", 1, logbook.warns);

	/* Latency now reports the ring's contents on top of the HAL's. */
	this->rate = 48000;
	check_int("the reported latency is what is still in the ring",
			(int64_t) RING_SIZE * SPA_NSEC_PER_SEC / (48000 * 4),
			(int64_t) latency_ns(this));

	this->hal_failed = true;
	io.status = SPA_STATUS_HAVE_DATA;
	check_int("once the HAL takes nothing the node stops filling the ring",
			SPA_STATUS_NEED_DATA, spa_node_process(&this->node));
	free_node(this);
}

static void test_capture_ring(void)
{
	struct impl *this;
	static struct testbuf t[2];
	struct spa_buffer *ptrs[2];
	struct spa_io_buffers io = { 0 };
	uint8_t payload[1024];
	uint32_t idx;

	section("handing captured audio to the graph");
	reset_all();
	this = make_node(true, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	this->quantum = 256;             /* 256 frames * 4 B = 1024 B per cycle */
	testbuf_init(&t[0], 4096);
	testbuf_init(&t[1], 4096);
	ptrs[0] = &t[0].buf;
	ptrs[1] = &t[1].buf;

	check_int("without a buffer area there is nowhere to write to", -EIO,
			spa_node_process(&this->node));
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &io, sizeof(io));
	io.status = SPA_STATUS_OK;
	io.buffer_id = SPA_ID_INVALID;
	check_int("with no buffers at all the graph is told the pipe is broken",
			SPA_STATUS_HAVE_DATA, spa_node_process(&this->node));
	check_int("as an error on the buffer area", -EPIPE, io.status);

	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 2);
	io.status = SPA_STATUS_OK;
	io.buffer_id = SPA_ID_INVALID;
	check_int("an empty ring still delivers a full buffer", SPA_STATUS_HAVE_DATA,
			spa_node_process(&this->node));
	check_int("of the size the graph expects", 1024, t[0].chunk.size);
	check_int("padded with silence rather than short", 1, this->n_underrun);
	check_int("and nothing was taken from the ring", 0, (int64_t) this->bytes_queued);

	check_int("while the graph has not taken that one, nothing new is filled",
			SPA_STATUS_HAVE_DATA, spa_node_process(&this->node));
	check_int("the same buffer is still the one on offer", 0, io.buffer_id);

	io.status = SPA_STATUS_OK;
	io.buffer_id = SPA_ID_INVALID;
	check_int("once it has, the second buffer goes out", SPA_STATUS_HAVE_DATA,
			spa_node_process(&this->node));
	check_int("as the second one", 1, io.buffer_id);

	io.status = SPA_STATUS_OK;
	io.buffer_id = SPA_ID_INVALID;
	check_int("with both buffers out there is nothing left to fill",
			SPA_STATUS_HAVE_DATA, spa_node_process(&this->node));
	check_int("which is a broken pipe as far as the graph is concerned",
			-EPIPE, io.status);

	/* Give one buffer back and put real data in the ring. */
	memset(payload, 0x5a, sizeof(payload));
	spa_ringbuffer_get_write_index(&this->ring, &idx);
	spa_ringbuffer_write_data(&this->ring, this->ring_data, RING_SIZE,
			idx & (RING_SIZE - 1), payload, sizeof(payload));
	spa_ringbuffer_write_update(&this->ring, idx + sizeof(payload));

	io.status = SPA_STATUS_OK;
	io.buffer_id = 0;
	this->n_underrun = 0;
	check_int("a returned buffer is filled from the ring", SPA_STATUS_HAVE_DATA,
			spa_node_process(&this->node));
	check("with the audio that was recorded", t[0].mem[0] == 0x5a &&
			t[0].mem[1023] == 0x5a);
	check_int("no padding needed", 0, this->n_underrun);
	check_int("and the ring is empty again", 1024, (int64_t) this->bytes_queued);
	free_node(this);
}

/* ----------------------------------------------------- writer and reader */

static bool wrote_once(void) { return hal_stub.writes >= 1; }
static bool wrote_three(void) { return hal_stub.writes >= 3; }
static bool read_once(void) { return hal_stub.reads >= 1; }

static struct impl *failing_node;
static bool gave_up(void) { return failing_node->hal_failed; }
static bool overran(void) { return failing_node->n_overrun > 0; }

static int start(struct impl *this)
{
	return spa_node_send_command(&this->node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Start));
}

static int pause_node(struct impl *this)
{
	return spa_node_send_command(&this->node,
			&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Pause));
}

static void test_writer_thread(void)
{
	struct impl *this;
	static struct testbuf t;
	struct spa_buffer *ptrs[1];
	struct spa_io_buffers io = { 0 };

	section("the thread that hands audio to the HAL");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;

	check_int("starting without a format is refused", -EIO, start(this));
	negotiate(this, 48000, 2);
	testbuf_init(&t, 4096);
	t.chunk.size = 4096;
	ptrs[0] = &t.buf;
	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 1);
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &io, sizeof(io));

	check_int("starting opens the HAL and the thread", 0, start(this));
	check("the stream is open", this->stream != NULL);
	check("the thread is running", this->started);

	io.status = SPA_STATUS_HAVE_DATA;
	io.buffer_id = 0;
	spa_node_process(&this->node);
	check("what the graph delivered reaches the HAL", wait_until(wrote_once));
	check_int("in one HAL period", 4096, (int64_t) hal_stub.bytes_written);

	check_int("pausing stops it", 0, pause_node(this));
	check("and the thread is gone", !this->started);
	check("the HAL stream stays open, ready for the next start",
			this->stream != NULL);

	check_int("starting again is fine", 0, start(this));
	check_int("but does not open a second stream", 1, hal_stub.output_opens);
	pause_node(this);
	free_node(this);
}

static void test_writer_drain(void)
{
	struct impl *this;
	static struct testbuf t;
	struct spa_buffer *ptrs[1];
	struct spa_io_buffers io = { 0 };

	section("the last few milliseconds of a song");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	testbuf_init(&t, 4096);
	t.chunk.size = 1000;             /* less than one HAL period */
	ptrs[0] = &t.buf;
	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 1);
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &io, sizeof(io));
	start(this);

	io.status = SPA_STATUS_HAVE_DATA;
	io.buffer_id = 0;
	spa_node_process(&this->node);
	check_int("a remainder smaller than a HAL period is not written yet", 0,
			hal_stub.writes);

	pause_node(this);
	check_int("stopping writes it out anyway", 1, hal_stub.writes);
	check_int("padded to a full period, so the end is not swallowed",
			4096, (int64_t) hal_stub.bytes_written);
	free_node(this);
}

static void test_writer_gives_up(void)
{
	struct impl *this;
	static struct testbuf t;
	struct spa_buffer *ptrs[1];
	struct spa_io_buffers io = { 0 };
	unsigned i;

	section("a HAL that stops taking data");
	reset_all();
	hal_stub.write_result = -EIO;
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	failing_node = this;
	negotiate(this, 48000, 2);
	testbuf_init(&t, 4096);
	t.chunk.size = 4096;
	ptrs[0] = &t.buf;
	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 1);
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &io, sizeof(io));
	start(this);

	for (i = 0; i < 4; i++) {
		io.status = SPA_STATUS_HAVE_DATA;
		io.buffer_id = 0;
		spa_node_process(&this->node);
	}
	check("three refusals in a row and the node gives up", wait_until(gave_up));
	check("rather than burning power on a stream that is gone",
			wait_until(wrote_three));
	check_int("one warning, then silence in the log", 1, logbook.warns);
	pause_node(this);
	check("and the whole run is summarised once", logbook.warns == 2);
	free_node(this);
}

static void test_reader_thread(void)
{
	struct impl *this;
	uint32_t idx;

	section("the thread that takes audio from the HAL");
	reset_all();
	this = make_node(true, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	check_int("starting opens the input and the thread", 0, start(this));
	check("something arrives", wait_until(read_once));
	pause_node(this);
	check("and lands in the ring",
			spa_ringbuffer_get_read_index(&this->ring, &idx) > 0);
	check_int("the node counted what it took", (int64_t) hal_stub.bytes_read,
			(int64_t) this->bytes_written);
	free_node(this);
}

static void test_reader_failures(void)
{
	struct impl *this;

	section("a microphone that delivers nothing");
	reset_all();
	hal_stub.read_result = 0;
	this = make_node(true, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	failing_node = this;
	negotiate(this, 48000, 2);
	start(this);
	check("three empty reads and the node gives up", wait_until(gave_up));
	pause_node(this);
	check_int("with one warning about the reads", 1,
			logbook.warns >= 1 ? 1 : 0);
	free_node(this);

	section("a microphone nobody listens to");
	reset_all();
	this = make_node(true, playback_info());
	failing_node = this;
	negotiate(this, 48000, 2);
	start(this);
	check("the ring fills up and the oldest audio is dropped",
			wait_until(overran));
	pause_node(this);
	free_node(this);
}

/* pthread_create returns its error, it does not set errno - reading errno
 * there once made a failed start look like a successful one. */
static void test_start_failure(void)
{
	struct impl *this;

	section("when the thread cannot be started");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	fail_thread_create = true;
	check_int("the start fails with the error the system gave", -EAGAIN,
			start(this));
	fail_thread_create = false;
	check("and leaves no stream open behind it", this->stream == NULL);
	check("nor a thread that is half there", !this->started);
	free_node(this);
}

/* --------------------------------------------------------- the clock */

static int on_ready(void *data, int status) { emitted.results++; return 0; }

static const struct spa_node_callbacks node_callbacks = {
	SPA_VERSION_NODE_CALLBACKS,
	.ready = on_ready,
};

static void test_clock(void)
{
	struct impl *this;
	struct spa_io_clock clock = { 0 };
	struct spa_io_position pos = { 0 };
	uint64_t first;

	section("the clock a hardware sink has to provide itself");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	spa_node_set_callbacks(&this->node, &node_callbacks, NULL);
	spa_node_set_io(&this->node, SPA_IO_Clock, &clock, sizeof(clock));
	clock.target_rate = SPA_FRACTION(1, 48000);
	hal_open(this);
	timer_start(this);

	check_int("one HAL period per tick", 4096 / 4, this->quantum);
	check_int("which at 48 kHz is 21333 us", 1024ull * SPA_NSEC_PER_SEC / 48000,
			(int64_t) this->period_ns);
	first = this->next_time;

	memset(&emitted, 0, sizeof(emitted));
	on_timeout(&this->timer_source);
	check_int("a tick that has not happened yet does nothing", 0, emitted.results);

	usleep((useconds_t) (this->period_ns / 1000) + 5000);
	on_timeout(&this->timer_source);
	check_int("a tick that has happened drives one graph cycle", 1, emitted.results);
	check_int("the clock says how long that cycle is", 1024, (int64_t) clock.duration);
	check("and when the next one is due", this->next_time > first);
	check("the delay it reports is the HAL's, in samples",
			clock.delay == (int64_t) (42ull * 48000 / 1000));

	/* The graph may pick another quantum at any time. */
	spa_node_set_io(&this->node, SPA_IO_Position, &pos, sizeof(pos));
	pos.clock.target_duration = 512;
	usleep((useconds_t) (this->period_ns / 1000) + 5000);
	on_timeout(&this->timer_source);
	check_int("a quantum the graph changed is taken over", 512, this->quantum);
	check_int("and the period with it", 512ull * SPA_NSEC_PER_SEC / 48000,
			(int64_t) this->period_ns);

	timer_stop(this);
	hal_close(this);
	free_node(this);
}

static void test_clock_without_hal(void)
{
	struct impl *this;
	struct spa_io_position pos = { 0 };

	section("the clock when the HAL gives no useful buffer size");
	reset_all();
	hal_stub.buffer_size = 0;
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	hal_open(this);
	timer_start(this);
	check_int("it falls back to 1024 frames", 1024, this->quantum);
	timer_stop(this);

	spa_node_set_io(&this->node, SPA_IO_Position, &pos, sizeof(pos));
	pos.clock.target_duration = 480;
	timer_start(this);
	check_int("unless the graph has already said what it wants", 480, this->quantum);
	timer_stop(this);
	hal_close(this);
	free_node(this);
}

/* ------------------------------------------- what WirePlumber sends over */

static void send_props(struct impl *this, const char *key, const char *val)
{
	uint8_t buf[512];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
	struct spa_pod_frame f[2];
	struct spa_pod *pod;

	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&b, &f[1]);
	spa_pod_builder_string(&b, key);
	spa_pod_builder_string(&b, val);
	spa_pod_builder_pop(&b, &f[1]);
	pod = spa_pod_builder_pop(&b, &f[0]);
	spa_node_set_param(&this->node, SPA_PARAM_Props, 0, pod);
}

/* The error paths of the reopen, which are the ones that matter when it goes
 * wrong at three in the morning.
 *
 * A reopen has to put back what it tore down: the writer thread and the clock.
 * If it cannot, the node must not be left looking healthy while playing
 * nothing - and if even the way back to the previous route fails, it has to
 * say so rather than sit there silently without a stream.
 */
static void test_reopen_when_things_fail(void)
{
	struct impl *this;

	section("a reopen that cannot put everything back");

	/* Crossing into Bluetooth while the node is RUNNING: the writer and the
	 * clock have to come back up on the other side. */
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	check_int("it starts", 0, start(this));
	check("the writer is running", this->started);
	check_int("crossing into Bluetooth while running", 0,
			apply_route(this, "output-bluetooth_sco"));
	check("and the writer is running again afterwards", this->started);
	check_str("on the Bluetooth port", "BT SCO", hal_stub.last_route);
	pause_node(this);
	hal_close(this);
	free_node(this);

	/* Neither route can be opened: the first attempt fails, and so does the
	 * way back. The node says it has no stream instead of pretending. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	hal_open(this);
	hal_stub.output_opens_failing = 2;   /* the move AND the way back */
	check_int("a crossing that fails both ways is reported", -EIO,
			apply_route(this, "output-bluetooth_sco"));
	check("and the node is honest about having no stream", this->stream == NULL);
	free_node(this);

	/* The stream reopens fine and the writer thread will not start again.
	 * Leaving the HAL open there would be a node holding hardware it cannot
	 * feed - the one state nothing else would clean up. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	check_int("it starts", 0, start(this));
	fail_thread_create = true;
	check("a reopen whose writer will not start is reported",
			apply_route(this, "output-bluetooth_sco") < 0);
	fail_thread_create = false;
	check("and it does not leave the HAL open behind it", this->stream == NULL);
	free_node(this);

	/* Same again one step later: the writer starts, the clock will not arm.
	 * Without the clock nothing calls process(), so the node would sit there
	 * with an open HAL stream and a thread waiting for audio that never
	 * comes. It has to undo both. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-speaker");
	check_int("it starts", 0, start(this));
	sys.settime_result = -EPERM;
	check("a reopen whose clock will not arm is reported",
			apply_route(this, "output-bluetooth_sco") < 0);
	sys.settime_result = 0;
	check("and that leaves no HAL stream either", this->stream == NULL);
	check("nor a running writer", !this->started);
	free_node(this);
}

/* Telling the HAL about the codec can fail too, and a capture node has its own
 * way into the Bluetooth port. */
static void test_bt_codec_edges(void)
{
	struct impl *this;

	section("the edges of the Bluetooth codec");

	/* The HAL refusing bt_wbs must not stop the stream from opening: a
	 * narrow-band link is still better than no audio, and the warning is
	 * what tells anyone why it sounds wrong. */
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	apply_bt_wbs(this, "on");
	apply_route(this, "output-bluetooth_sco");
	hal_stub.set_parameters_result = -EIO;
	check_int("the stream opens even when the HAL refuses the codec", 0,
			hal_open(this));
	hal_stub.set_parameters_result = 0;
	hal_close(this);
	free_node(this);

	/* The capture side reaches the Bluetooth microphone through the same
	 * remembered route, and has to announce the codec the same way. */
	reset_all();
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	apply_bt_wbs(this, "on");
	check_int("a capture node takes the Bluetooth route", 0,
			apply_route(this, "input-bluetooth_sco_headset"));
	hal_stub.all_parameters[0] = '\0';
	check_int("and opens on it", 0, hal_open(this));
	check("with the codec announced", strstr(hal_stub.all_parameters, "bt_wbs=on") != NULL);
	check_str("on the headset microphone", "BT SCO Headset Mic",
			hal_stub.last_input_device);
	hal_close(this);
	free_node(this);

	/* And the way it actually arrives in the daemon: as a node prop, which is
	 * how droid-bluetooth-call.lua sends it. */
	reset_all();
	this = make_node(false, playback_info());
	send_props(this, "droid.bt-wbs", "off");
	check_str("the codec arrives as a node prop", "off", this->bt_wbs);
	send_props(this, "droid.bt-wbs", "neither");
	check_str("and nonsense leaves the last good one alone", "off", this->bt_wbs);
	free_node(this);

	/* A route the configuration no longer has. It used to fall through to the
	 * speaker without a word, which is how a Bluetooth call spent days
	 * playing out of the phone; now it warns and falls back openly. The port
	 * name remembered alongside it is what the fallback tries first. */
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	apply_route(this, "output-earpiece");
	snprintf(this->wanted_route, sizeof(this->wanted_route), "%s", "output-gone");
	check_int("a route that vanished still opens something", 0, hal_open(this));
	check_str("by falling back on the port it remembered", "Earpiece",
			hal_stub.last_route);
	hal_close(this);

	/* A capture node whose remembered route cannot be resolved: the codec
	 * announcement has nothing to announce for and must simply keep quiet
	 * rather than reach into a null port. */
	{
		struct impl *cap = make_node(true, playback_info());
		negotiate(cap, 48000, 2);
		snprintf(cap->wanted_route, sizeof(cap->wanted_route), "%s", "input-gone");
		check_int("a capture node with a vanished route still opens", 0,
				hal_open(cap));
		hal_close(cap);
		free_node(cap);
	}

	/* And with neither of them resolvable, the default output. */
	snprintf(this->wanted_route, sizeof(this->wanted_route), "%s", "output-gone");
	this->wanted_port[0] = '\0';
	check_int("with nothing left to go on, the default output", 0, hal_open(this));
	hal_close(this);
	free_node(this);
}

static void test_set_param(void)
{
	struct impl *this;
	uint8_t buf[512];
	struct spa_pod_builder b;
	struct spa_pod_frame f[2];
	struct spa_pod *pod;

	section("the route, the mode and the call volume, as they arrive");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);

	send_props(this, "droid.route", "output-earpiece");
	check_str("a route arrives", "Earpiece", this->wanted_port);
	send_props(this, "droid.mode", "call");
	check_int("a mode arrives", AUDIO_MODE_IN_CALL, hal_stub.mode);
	send_props(this, "droid.voice-volume", "0.75");
	check("and the call volume", fabsf(hal_stub.voice_volume - 0.75f) < 0.001f);
	send_props(this, "droid.something-else", "1");
	check_str("a key the node does not know changes nothing",
			"Earpiece", this->wanted_port);
	send_props(this, "droid.mode", "normal");

	check_int("a parameter that is not Props is refused", -ENOENT,
			spa_node_set_param(&this->node, SPA_PARAM_Route, 0, NULL));
	check_int("and Props without a body too", -ENOENT,
			spa_node_set_param(&this->node, SPA_PARAM_Props, 0, NULL));

	/* Props with other contents must be stepped over, not tripped on. */
	spa_pod_builder_init(&b, buf, sizeof(buf));
	pod = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_volume, SPA_POD_Float(0.5f));
	check_int("Props the node has no use for are ignored", 0,
			spa_node_set_param(&this->node, SPA_PARAM_Props, 0, pod));

	spa_pod_builder_init(&b, buf, sizeof(buf));
	pod = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props,
			SPA_PROP_params, SPA_POD_Int(7));
	check_int("and so is a params pair that is not a pair at all", 0,
			spa_node_set_param(&this->node, SPA_PARAM_Props, 0, pod));

	spa_pod_builder_init(&b, buf, sizeof(buf));
	spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
	spa_pod_builder_prop(&b, SPA_PROP_params, 0);
	spa_pod_builder_push_struct(&b, &f[1]);
	spa_pod_builder_string(&b, "droid.route");
	spa_pod_builder_pop(&b, &f[1]);
	pod = spa_pod_builder_pop(&b, &f[0]);
	check_int("a key without a value is dropped rather than read past the end",
			0, spa_node_set_param(&this->node, SPA_PARAM_Props, 0, pod));
	free_node(this);
}

/* ------------------------------------------------------------ commands */

static void test_commands(void)
{
	struct impl *this;

	section("start, pause, suspend");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	start(this);
	check_int("suspend gives the hardware back, so PulseAudio can have it", 0,
			spa_node_send_command(&this->node,
				&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend)));
	check("the stream is closed", this->stream == NULL);

	start(this);
	apply_mode(this, "call");
	check_int("but during a call it does not", 0,
			spa_node_send_command(&this->node,
				&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Suspend)));
	check("the voice path needs the HAL even with nothing playing",
			this->stream != NULL);
	apply_mode(this, "normal");

	check_int("a command the node does not know is refused", -ENOTSUP,
			spa_node_send_command(&this->node,
				&SPA_NODE_COMMAND_INIT(SPA_NODE_COMMAND_Flush)));
	free_node(this);
}

static void test_teardown(void)
{
	struct impl *this;

	section("tearing the node down while it is running");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	start(this);
	check("it is running", this->started);
	spa_handle_clear(&this->handle);
	check("clearing the handle stops the thread", !this->started);
	check("closes the stream", this->stream == NULL);
	check("gives the timer back", sys.closes >= 1);
	check("and takes the timer out of the loop", sys.removes >= 1);
	check_int("and it is no longer reachable from the device", -ENOENT,
			droid_node_set_route("primary output", "output-speaker"));
	free(this);
	hw_module_keepalive = NULL;

	section("a format taken away under a running stream");
	reset_all();
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	start(this);
	spa_node_port_set_param(&this->node, this->dir, 0, SPA_PARAM_Format, 0, NULL);
	check("the clock stops with the format", !this->started);
	check("and the hardware goes back", this->stream == NULL);
	free_node(this);
}


/* ------------------------------------------ the corners that only a phone
 * reaches: no memory, no timer, no thread, one channel */

static void test_mono(void)
{
	struct impl *this;

	section("mono, which is what the voice path runs in");
	reset_all();
	hal_stub.rate = 16000;
	hal_stub.channels = 1;
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 16000, 1);
	check_int("a mono output stream opens", 0, hal_open(this));
	hal_close(this);
	free_node(this);

	reset_all();
	hal_stub.rate = 16000;
	hal_stub.channels = 1;
	this = make_node(true, playback_info());
	negotiate(this, 16000, 1);
	check_int("and a mono input too", 0, hal_open(this));
	hal_stub.reconfigure_calls = 0;
	apply_mode(this, "normal");
	check_int("whose audio source is restored after a call just the same", 1,
			hal_stub.reconfigure_calls);
	hal_close(this);
	free_node(this);
}

static void test_keepalive(void)
{
	struct impl *this;

	section("the HAL module that is never given back");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	/* As if the up-front load in impl_init had not worked - then the first
	 * stream has to take the reference, or libhybris faults on the second
	 * open. */
	hw_module_keepalive = NULL;
	hal_stub.module_refs = 0;
	check_int("the stream opens", 0, hal_open(this));
	check_int("and holds on to the module itself", 1, hal_stub.module_refs);
	check("so it survives the stream being closed", hw_module_keepalive != NULL);
	hal_close(this);
	free_node(this);
}

static void test_no_memory(void)
{
	struct spa_handle *h;
	struct impl *this;
	struct spa_dict d = *playback_info();
	static struct testbuf oom_buf;
	static struct spa_buffer *oom_ptrs[1];
	static struct spa_io_buffers oom_io;

	section("when there is no memory left");
	reset_all();
	h = calloc(1, sizeof(struct impl));
	fail_malloc_size = RING_SIZE;
	check_int("a node without a ring buffer refuses to come up", -ENOMEM,
			impl_init(&droid_pcm_factory, h, &d, support, SPA_N_ELEMENTS(support)));
	fail_malloc_size = 0;
	free(h);
	hw_module_keepalive = NULL;

	reset_all();
	hal_stub.buffer_size = 3333;   /* an odd size, so only this one fails */
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	testbuf_init(&oom_buf, 4096);
	oom_buf.chunk.size = 4096;
	oom_ptrs[0] = &oom_buf.buf;
	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, oom_ptrs, 1);
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &oom_io,
			sizeof(oom_io));
	fail_malloc_size = 3333;
	check_int("a writer thread without a buffer of its own still starts", 0,
			start(this));
	oom_io.status = SPA_STATUS_HAVE_DATA;
	oom_io.buffer_id = 0;
	spa_node_process(&this->node);
	check("but writes nothing, ever", never(wrote_once, 200));
	pause_node(this);           /* joins the thread while it can still fail */
	fail_malloc_size = 0;
	check_int("not even the remainder when stopping", 0, hal_stub.writes);
	free_node(this);

	reset_all();
	hal_stub.buffer_size = 3333;
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	fail_malloc_size = 3333;
	check_int("and the same on the reading side", 0, start(this));
	check("nothing is read", never(read_once, 200));
	pause_node(this);
	fail_malloc_size = 0;
	check_int("nothing at all", 0, hal_stub.reads);
	free_node(this);
}

static void test_odd_buffer_sizes(void)
{
	struct impl *this;

	section("a HAL buffer size the threads cannot work with");
	reset_all();
	hal_stub.buffer_size = 0;
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	check_int("the writer falls back to 4 kB at a time", 0, start(this));
	check("and runs", this->started);
	pause_node(this);
	free_node(this);

	reset_all();
	hal_stub.buffer_size = 0;
	this = make_node(true, playback_info());
	negotiate(this, 48000, 2);
	check_int("and so does the reader", 0, start(this));
	check("which then reads in 4 kB blocks", wait_until(read_once));
	pause_node(this);
	free_node(this);

	reset_all();
	hal_stub.buffer_size = 2;   /* less than one stereo frame */
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	hal_open(this);
	timer_start(this);
	check_int("and the clock falls back to 1024 frames", 1024, this->quantum);
	timer_stop(this);
	hal_close(this);
	free_node(this);
}

static void test_timer_failures(void)
{
	struct spa_handle *h;
	struct impl *this;
	struct spa_dict d = *playback_info();

	section("when the timer will not play along");
	reset_all();
	h = calloc(1, sizeof(struct impl));
	sys.timerfd_create_result = -EMFILE;
	check_int("no timer, no node - it would look healthy and play nothing",
			-EMFILE,
			impl_init(&droid_pcm_factory, h, &d, support, SPA_N_ELEMENTS(support)));
	sys.timerfd_create_result = 0;
	free(h);
	hw_module_keepalive = NULL;

	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	sys.settime_result = -EIO;
	check_int("a timer that cannot be armed fails the start", -EIO, start(this));
	sys.settime_result = 0;
	check("with no thread left running", !this->started);
	check("and the hardware given back", this->stream == NULL);
	free_node(this);
}

static void test_start_edge_cases(void)
{
	struct impl *this;

	section("starting twice, and starting without hardware");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	check_int("the first start runs", 0, start(this));
	check_int("the second changes nothing", 0, start(this));
	check_int("and there is still one thread", 1, hal_stub.output_opens);
	pause_node(this);
	check_int("pausing something that is not running is harmless", 0,
			pause_node(this));
	free_node(this);

	reset_all();
	hal_stub.module_works = false;
	this = make_node(false, playback_info());
	negotiate(this, 48000, 2);
	check_int("no HAL, no start", -EIO, start(this));
	free_node(this);
}

static void test_underrun_summary(void)
{
	struct impl *this;
	static struct testbuf t;
	struct spa_buffer *ptrs[1];
	struct spa_io_buffers io = { 0 };

	section("a capture run that had to invent silence");
	reset_all();
	hal_stub.read_result = 0;     /* the HAL delivers nothing at all */
	this = make_node(true, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	negotiate(this, 48000, 2);
	testbuf_init(&t, 4096);
	ptrs[0] = &t.buf;
	spa_node_port_use_buffers(&this->node, this->dir, 0, 0, ptrs, 1);
	spa_node_port_set_io(&this->node, this->dir, 0, SPA_IO_Buffers, &io, sizeof(io));
	start(this);
	check("the HAL is asked and gives nothing", wait_until(read_once));
	io.status = SPA_STATUS_OK;
	io.buffer_id = SPA_ID_INVALID;
	spa_node_process(&this->node);
	check_int("the graph still gets a full buffer", 1, this->n_underrun);
	pause_node(this);
	check("and the run says so afterwards", logbook.warns >= 1);
	free_node(this);
}

static void test_more_params(void)
{
	struct impl *this;
	struct spa_hook listener = { 0 };
	struct spa_io_position pos = { 0 };
	const struct spa_pod_prop *p;
	uint8_t buf[512];
	struct spa_pod_builder b;
	int32_t size = 0;

	section("the last corners of the parameter handling");
	reset_all();
	this = make_node(false, playback_info());
	if (!check("the node is there", this != NULL))
		return;
	spa_node_add_listener(&this->node, &listener, &node_events, NULL);
	negotiate(this, 48000, 2);

	memset(&emitted, 0, sizeof(emitted));
	check_int("asking for two formats when there is one stops at the end", 0,
			spa_node_port_enum_params(&this->node, 0, this->dir, 0,
				SPA_PARAM_EnumFormat, 0, 2, NULL));
	check_int("having emitted the one there is", 1, emitted.results);

	this->quantum = 0;
	spa_node_set_io(&this->node, SPA_IO_Position, &pos, sizeof(pos));
	pos.clock.target_duration = 2048;
	enum_params(this, SPA_PARAM_Buffers, NULL);
	p = spa_pod_find_prop(emitted.last_param, NULL, SPA_PARAM_BUFFERS_size);
	if (p)
		spa_pod_get_int(&p->value, &size);
	check_int("before the first cycle the graph's own quantum decides",
			2048 * 4, size);

	spa_pod_builder_init(&b, buf, sizeof(buf));
	{
		struct spa_pod *bad = spa_pod_builder_add_object(&b,
			SPA_TYPE_OBJECT_Format, SPA_PARAM_Format,
			SPA_FORMAT_mediaType,     SPA_POD_Id(SPA_MEDIA_TYPE_audio),
			SPA_FORMAT_mediaSubtype,  SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
			SPA_FORMAT_AUDIO_format,  SPA_POD_Id(SPA_AUDIO_FORMAT_S16_LE),
			SPA_FORMAT_AUDIO_rate,    SPA_POD_Int(48000),
			SPA_FORMAT_AUDIO_channels, SPA_POD_Int(9999));
		check_int("raw audio that does not read back at all is refused", -EINVAL,
				spa_node_port_set_param(&this->node, this->dir, 0,
					SPA_PARAM_Format, 0, bad));
	}

	this->module = NULL;
	check("with no configuration at all no route can be looked up",
			port_by_route_name(this, "output-speaker") == NULL);

	spa_hook_remove(&listener);
	free_node(this);
}

int main(void)
{
	/* The configuration parser announces every file it reads on stderr, and
	 * this test opens forty of them. Nothing is asserted on that output, so
	 * it only makes the checks hard to read - set DROID_TEST_VERBOSE to see
	 * it when something is wrong. */
	if (!getenv("DROID_TEST_VERBOSE") &&
	    freopen("/dev/null", "w", stderr) == NULL)
		return 1;

	printf("droid-pcm\n");

	fixture_config = pa_parse_droid_audio_config(TEST_FIXTURE);
	if (!fixture_config ||
	    !(fixture_module = dm_config_find_module(fixture_config, "primary"))) {
		printf("  \033[31mFAIL\033[0m the fixture does not parse\n");
		return 1;
	}
	reset_all();

	test_factories();
	test_init_defaults();
	test_init_capture();
	test_init_properties();
	test_init_bad_properties();
	test_init_refusals();
	test_hal_open_playback();
	test_hal_open_playback_variants();
	test_hal_open_failures();
	test_hal_open_capture();
	test_hal_open_capture_ports();
	test_route_names();
	test_apply_route();
	test_bluetooth_reopen();
	test_bt_codec();
	test_reopen_when_things_fail();
	test_bt_codec_edges();
	test_registry();
	test_apply_mode();
	test_apply_mode_details();
	test_audio_source_after_call();
	test_voice_volume();
	test_listener_and_params();
	test_buffer_size();
	test_set_format();
	test_io_areas();
	test_use_buffers();
	test_playback_ring();
	test_capture_ring();
	test_writer_thread();
	test_writer_drain();
	test_writer_gives_up();
	test_reader_thread();
	test_reader_failures();
	test_start_failure();
	test_clock();
	test_clock_without_hal();
	test_set_param();
	test_commands();
	test_teardown();
	test_mono();
	test_keepalive();
	test_no_memory();
	test_odd_buffer_sizes();
	test_timer_failures();
	test_start_edge_cases();
	test_underrun_summary();
	test_more_params();

	printf("\n  %d checks, %d failed\n", checks, failures);
	fflush(stdout);
	return failures == 0 ? 0 : 1;
}
