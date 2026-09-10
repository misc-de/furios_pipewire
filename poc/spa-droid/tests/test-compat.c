/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* The PulseAudio functions we had to write ourselves.
 *
 * These replaced libpulse - fourteen small things the ported droid code calls
 * for channel maps, sample formats, string handling and a couple of
 * containers. They are pure: no HAL, no graph, nothing to mock. Which makes
 * them the cheapest coverage in the project, and also the least forgiving: a
 * wrong frame size or a hash that collides shows up as distorted audio three
 * layers away, with nothing pointing back here.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>
#include "pulsecore/core.h"
#include "pulsecore/core-util.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"
#include "pulsecore/strbuf.h"
#include "pulsecore/strlist.h"
#include "pulsecore/shared.h"
#include "pulsecore/modargs.h"
#include "pulsecore/mutex.h"

static int failures;
static int checks;

static void ok(const char *what)
{
	checks++;
	printf("  \033[32mok\033[0m   %s\n", what);
}

static void bad(const char *what, const char *detail)
{
	checks++;
	failures++;
	printf("  \033[31mFAIL\033[0m %s: %s\n", what, detail);
}

static void check(const char *what, bool cond)
{
	if (cond)
		ok(what);
	else
		bad(what, "condition does not hold");
}

static void check_uint(const char *what, unsigned long want, unsigned long got)
{
	if (want == got) {
		ok(what);
	} else {
		char buf[128];
		snprintf(buf, sizeof(buf), "expected %lu, got %lu", want, got);
		bad(what, buf);
	}
}

static void check_str(const char *what, const char *want, const char *got)
{
	if (got != NULL && strcmp(want, got) == 0) {
		ok(what);
	} else {
		char buf[256];
		snprintf(buf, sizeof(buf), "expected \"%s\", got \"%s\"", want,
				got ? got : "(null)");
		bad(what, buf);
	}
}

/* --- sample formats and frames ------------------------------------------
 *
 * The frame size decides how many bytes one moment of audio takes. Getting it
 * wrong does not fail: it plays at the wrong speed, or in the wrong channel.
 */
static void test_sample(void)
{
	pa_sample_spec s16 = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2 };
	pa_sample_spec mono = { .format = PA_SAMPLE_S16LE, .rate = 16000, .channels = 1 };
	pa_sample_spec broken = { .format = PA_SAMPLE_S16LE, .rate = 0, .channels = 2 };

	printf("sample formats\n");
	check_uint("16-bit samples are two bytes", 2,
			pa_sample_size_of_format(PA_SAMPLE_S16LE));
	check_uint("8-bit samples are one", 1, pa_sample_size_of_format(PA_SAMPLE_U8));
	check_uint("32-bit floats are four", 4,
			pa_sample_size_of_format(PA_SAMPLE_FLOAT32LE));
	check_uint("a stereo 16-bit frame is four bytes", 4, pa_frame_size(&s16));
	check_uint("a mono one is two", 2, pa_frame_size(&mono));
	check_uint("sample size ignores the channel count", 2, pa_sample_size(&s16));

	/* 48000 stereo frames of 4 bytes is one second. */
	check_uint("bytes convert to time", 1000000,
			(unsigned long) pa_bytes_to_usec(48000 * 4, &s16));
	check_uint("and half of it to half a second", 500000,
			(unsigned long) pa_bytes_to_usec(24000 * 4, &s16));

	/* Out-of-range formats: the ported code passes whatever the HAL reports,
	 * and a table read past its end is not a bug anyone would spot later. */
	check_uint("a format below the table is zero-sized", 0,
			pa_sample_size_of_format((pa_sample_format_t) -1));
	check_uint("and one past its end too", 0,
			pa_sample_size_of_format((pa_sample_format_t) PA_SAMPLE_MAX));
	check_str("a known format has a name", "s16le",
			pa_sample_format_to_string(PA_SAMPLE_S16LE));
	check("an unknown one has none",
			pa_sample_format_to_string((pa_sample_format_t) PA_SAMPLE_MAX) == NULL);
	check("and neither does a negative one",
			pa_sample_format_to_string((pa_sample_format_t) -1) == NULL);

	/* Nothing at all, which is what a caller passes on a path that failed
	 * earlier. None of these may reach into the null pointer. */
	check_uint("no spec has no frame size", 0, pa_frame_size(NULL));
	check_uint("and converts to no time", 0,
			(unsigned long) pa_bytes_to_usec(4096, NULL));
	check_uint("nor does a spec with no rate", 0,
			(unsigned long) pa_bytes_to_usec(4096, &broken));
	{
		pa_sample_spec no_format = { .format = (pa_sample_format_t) PA_SAMPLE_MAX,
			.rate = 48000, .channels = 2 };
		pa_sample_spec no_channels = { .format = PA_SAMPLE_S16LE,
			.rate = 48000, .channels = 0 };
		pa_sample_spec too_fast = { .format = PA_SAMPLE_S16LE,
			.rate = PA_RATE_MAX + 1, .channels = 2 };

		check_uint("a frame of an unknown format is nothing", 0,
				(unsigned long) pa_bytes_to_usec(4096, &no_format));
		check("nothing is not a valid spec", !pa_sample_spec_valid(NULL));
		check("nor one with no channels", !pa_sample_spec_valid(&no_channels));
		check("nor one with an impossible rate", !pa_sample_spec_valid(&too_fast));
		check("nor one with a format that does not exist",
				!pa_sample_spec_valid(&no_format));
	}

	check("a sane spec is valid", pa_sample_spec_valid(&s16));
	check("one without a rate is not", !pa_sample_spec_valid(&broken));
	check("a spec equals itself", pa_sample_spec_equal(&s16, &s16));
	check("and differs from another", !pa_sample_spec_equal(&s16, &mono));
	check("nothing does not equal something", !pa_sample_spec_equal(NULL, &s16));
	check("and something does not equal nothing", !pa_sample_spec_equal(&s16, NULL));
}

/* --- channel maps --------------------------------------------------------
 *
 * The map says which channel is which. Comparing two of them is what the
 * ported code uses to decide whether it can play something as it is.
 */
static void test_channel_map(void)
{
	pa_channel_map mono, stereo, other;
	char buf[PA_CHANNEL_MAP_SNPRINT_MAX];

	printf("\nchannel maps\n");
	pa_channel_map_init_mono(&mono);
	pa_channel_map_init_stereo(&stereo);
	pa_channel_map_init_stereo(&other);

	check("initialising nothing as mono returns nothing",
			pa_channel_map_init_mono(NULL) == NULL);
	check("and the same for stereo",
			pa_channel_map_init_stereo(NULL) == NULL);
	check_uint("mono has one channel", 1, mono.channels);
	check_uint("stereo has two", 2, stereo.channels);
	check("a mono map is valid", pa_channel_map_valid(&mono));
	check("a stereo map is valid", pa_channel_map_valid(&stereo));
	check("two stereo maps are equal", pa_channel_map_equal(&stereo, &other));
	check("mono and stereo are not", !pa_channel_map_equal(&mono, &stereo));

	other.map[0] = PA_CHANNEL_POSITION_REAR_LEFT;
	check("nor are two maps with different positions",
			!pa_channel_map_equal(&stereo, &other));
	check("a map equals itself without comparing anything",
			pa_channel_map_equal(&stereo, &stereo));
	check("nothing is not a valid map", !pa_channel_map_valid(NULL));

	{
		pa_channel_map empty = { 0 };
		pa_channel_map too_many, bad_position;

		check("a map with no channels is not valid",
				!pa_channel_map_valid(&empty));

		too_many = stereo;
		too_many.channels = PA_CHANNELS_MAX + 1;
		check("nor one with more channels than exist",
				!pa_channel_map_valid(&too_many));

		bad_position = stereo;
		bad_position.map[1] = (pa_channel_position_t) PA_CHANNEL_POSITION_MAX;
		check("nor one naming a position that does not exist",
				!pa_channel_map_valid(&bad_position));

		check("printing into no buffer returns it unchanged",
				pa_channel_map_snprint(NULL, 16, &stereo) == NULL);
		check("printing with no room returns the buffer unchanged",
				pa_channel_map_snprint(buf, 0, &stereo) == buf);
		pa_channel_map_snprint(buf, sizeof(buf), &empty);
		check_str("an empty map prints as invalid rather than as nothing",
				"(invalid)", buf);
	}

	check("a map prints something readable",
			pa_channel_map_snprint(buf, sizeof(buf), &stereo) != NULL &&
			buf[0] != '\0');
}

/* --- the property list ---------------------------------------------------
 *
 * Four functions and one job: carry the Android audio source to the HAL. If
 * this drops a value the microphone quietly records the wrong thing.
 */
static void test_proplist(void)
{
	pa_proplist *pl = pa_proplist_new();

	printf("\nproperty list\n");
	check("it is created", pl != NULL);
	check("setting on nothing is refused",
			pa_proplist_sets(NULL, "k", "v") < 0);
	check("reading from nothing gives nothing",
			pa_proplist_gets(NULL, "k") == NULL);
	pa_proplist_free(NULL);
	ok("freeing nothing is harmless");
	check_uint("a value can be set", 0,
			(unsigned long) pa_proplist_sets(pl, "droid.audio_source", "mic"));
	check_str("and read back", "mic",
			pa_proplist_gets(pl, "droid.audio_source"));
	pa_proplist_sets(pl, "droid.audio_source", "voice communication");
	check_str("and overwritten", "voice communication",
			pa_proplist_gets(pl, "droid.audio_source"));
	check("a key that was never set reads as nothing",
			pa_proplist_gets(pl, "nothing.here") == NULL);
	pa_proplist_free(pl);
}

/* --- hash map ------------------------------------------------------------ */
static void test_hashmap(void)
{
	pa_hashmap *h = pa_hashmap_new(pa_idxset_string_hash_func,
			pa_idxset_string_compare_func);
	int a = 1, b = 2;
	void *state = NULL;
	const void *key;
	unsigned seen = 0;

	printf("\nhash map\n");
	check("it is created", h != NULL);
	check_uint("it starts empty", 0, pa_hashmap_size(h));
	pa_hashmap_put(h, (void *) "one", &a);
	pa_hashmap_put(h, (void *) "two", &b);
	check_uint("two entries went in", 2, pa_hashmap_size(h));
	check("and come back out", pa_hashmap_get(h, "one") == &a);
	check("both of them", pa_hashmap_get(h, "two") == &b);
	check("a key that is not there returns nothing",
			pa_hashmap_get(h, "three") == NULL);

	while (pa_hashmap_iterate(h, &state, &key) != NULL)
		seen++;
	check_uint("iteration walks every entry", 2, seen);

	check("removing gives the value back", pa_hashmap_remove(h, "one") == &a);
	check_uint("and the map shrinks", 1, pa_hashmap_size(h));
	check("removing what is gone returns nothing",
			pa_hashmap_remove(h, "one") == NULL);
	pa_hashmap_free(h);

	/* Nothing at all, on every entry point: the ported code calls these on
	 * paths where an earlier step already failed. */
	check_uint("no map has no size", 0, pa_hashmap_size(NULL));
	check("reading from nothing gives nothing", pa_hashmap_get(NULL, "k") == NULL);
	check("removing from nothing gives nothing",
			pa_hashmap_remove(NULL, "k") == NULL);
	check("putting into nothing is refused", pa_hashmap_put(NULL, (void *) "k", &a) < 0);
	pa_hashmap_free(NULL);
	ok("freeing nothing is harmless");

	/* A hash map that grows past its first allocation - the resize path is
	 * the one that quietly corrupts things when it is wrong. */
	{
		pa_hashmap *big = pa_hashmap_new(pa_idxset_string_hash_func,
				pa_idxset_string_compare_func);
		static char keys[64][8];
		unsigned i, found = 0;

		for (i = 0; i < 64; i++) {
			snprintf(keys[i], sizeof(keys[i]), "k%u", i);
			pa_hashmap_put(big, keys[i], &keys[i]);
		}
		check_uint("sixty-four entries all went in", 64, pa_hashmap_size(big));
		for (i = 0; i < 64; i++)
			if (pa_hashmap_get(big, keys[i]) == &keys[i])
				found++;
		check_uint("and every one of them comes back", 64, found);
		pa_hashmap_free(big);
	}

	/* Storing the same key twice replaces rather than duplicates. */
	{
		pa_hashmap *h2 = pa_hashmap_new(pa_idxset_string_hash_func,
				pa_idxset_string_compare_func);
		pa_hashmap_put(h2, (void *) "same", &a);
		pa_hashmap_put(h2, (void *) "same", &b);
		check_uint("the same key twice is still one entry", 1, pa_hashmap_size(h2));
		pa_hashmap_free(h2);
	}
}

/* --- indexed set --------------------------------------------------------- */
static unsigned freed;

static void count_free(void *p)
{
	freed++;
	free(p);
}

static void test_idxset(void)
{
	pa_idxset *s = pa_idxset_new(pa_idxset_trivial_hash_func,
			pa_idxset_trivial_compare_func);
	int a = 1, b = 2;
	uint32_t idx_a = PA_IDXSET_INVALID, idx_b = PA_IDXSET_INVALID;
	uint32_t state = PA_IDXSET_INVALID;
	void *p;
	unsigned seen = 0;

	printf("\nindexed set\n");
	check("it is created", s != NULL);
	check("it starts empty", pa_idxset_isempty(s));
	pa_idxset_put(s, &a, &idx_a);
	pa_idxset_put(s, &b, &idx_b);
	check_uint("two entries went in", 2, pa_idxset_size(s));
	check("they got different indices", idx_a != idx_b);
	check("and are no longer empty", !pa_idxset_isempty(s));
	check("an entry is found by its data", pa_idxset_get_by_data(s, &a, NULL) == &a);

	for (p = pa_idxset_first(s, &state); p != NULL; p = pa_idxset_next(s, &state))
		seen++;
	check_uint("iteration walks every entry", 2, seen);

	check("removing gives the value back", pa_idxset_remove_by_data(s, &a, NULL) == &a);
	check_uint("and the set shrinks", 1, pa_idxset_size(s));

	/* The trivial hash is for sets keyed by pointer - the droid code uses it
	 * for the streams it keeps. It must be stable; it is explicitly allowed
	 * to collide, and does, since it shifts the pointer by four bits and two
	 * neighbouring stack variables land on the same value. The set falls back
	 * on the compare function for that, which is the part that has to be
	 * exact. */
	check("the trivial hash is stable for the same pointer",
			pa_idxset_trivial_hash_func(&a) == pa_idxset_trivial_hash_func(&a));
	check("comparing a pointer with itself says equal",
			pa_idxset_trivial_compare_func(&a, &a) == 0);
	check("and with another says different",
			pa_idxset_trivial_compare_func(&a, &b) != 0);
	check("the string hash is stable",
			pa_idxset_string_hash_func("primary") ==
			pa_idxset_string_hash_func("primary"));
	check("and comparing strings goes by content, not address",
			pa_idxset_string_compare_func("primary", "primary") == 0);

	/* Putting the same thing twice reports the index it already has, rather
	 * than adding it again. */
	{
		uint32_t existing = PA_IDXSET_INVALID;
		check("putting the same entry again is refused",
				pa_idxset_put(s, &b, &existing) < 0);
		check("and it says which index it already has", existing == idx_b);
	}

	check("looking for something that is not there gives nothing",
			pa_idxset_get_by_data(s, &a, NULL) == NULL);
	check("removing something that is not there gives nothing",
			pa_idxset_remove_by_data(s, &a, NULL) == NULL);

	check_uint("no set has no size", 0, pa_idxset_size(NULL));
	check("nothing is empty", pa_idxset_isempty(NULL));
	check("putting into nothing is refused", pa_idxset_put(NULL, &a, NULL) < 0);
	check("reading from nothing gives nothing",
			pa_idxset_get_by_data(NULL, &a, NULL) == NULL);
	check("removing from nothing gives nothing",
			pa_idxset_remove_by_data(NULL, &a, NULL) == NULL);
	check("iterating nothing gives nothing",
			pa_idxset_first(NULL, &state) == NULL);

	pa_idxset_free(s, NULL);

	/* With a free callback, which is how the droid code releases what it
	 * stored. */
	{
		pa_idxset *owned = pa_idxset_new(pa_idxset_trivial_hash_func,
				pa_idxset_trivial_compare_func);
		int *first = malloc(sizeof(int));
		int *second = malloc(sizeof(int));

		freed = 0;
		pa_idxset_put(owned, first, NULL);
		pa_idxset_put(owned, second, NULL);
		pa_idxset_free(owned, count_free);
		check_uint("freeing a set with a callback releases every entry", 2, freed);
	}
	pa_idxset_free(NULL, NULL);
	ok("freeing nothing is harmless");
}

/* --- strings ------------------------------------------------------------- */
static void test_strings(void)
{
	pa_strbuf *sb = pa_strbuf_new();
	const char *csv = "one,two,three";
	const char *state = NULL;
	char *part, *joined, *replaced;
	unsigned parts = 0;
	pa_strlist *sl;

	printf("\nstrings\n");
	pa_strbuf_puts(sb, "hello");
	pa_strbuf_putsn(sb, " world and more", 6);
	/* Enough text to force the buffer to grow past its first allocation. */
	{
		unsigned i;
		for (i = 0; i < 200; i++)
			pa_strbuf_puts(sb, "0123456789");
	}
	joined = pa_strbuf_to_string_free(sb);
	check("a string buffer joins pieces, honouring a length and growing as needed",
			joined != NULL && strncmp(joined, "hello world", 11) == 0 &&
			strlen(joined) == 11 + 2000);
	free(joined);
	pa_strbuf_free(pa_strbuf_new());
	ok("a buffer can be thrown away unused");
	pa_strbuf_free(NULL);
	ok("and freeing nothing is harmless");

	while ((part = pa_split(csv, ",", &state)) != NULL) {
		parts++;
		free(part);
	}
	check_uint("splitting finds every field", 3, parts);

	state = NULL;
	part = pa_split_spaces("alpha beta", &state);
	check_str("splitting on spaces gives the first word", "alpha", part);
	free(part);

	replaced = pa_replace("a-b-c", "-", "+");
	check_str("replacing swaps every occurrence", "a+b+c", replaced);
	free(replaced);

	replaced = pa_replace("nothing here", "xyz", "!");
	check_str("and leaves a string without the needle alone", "nothing here",
			replaced);
	free(replaced);

	sl = pa_strlist_prepend(NULL, "second");
	sl = pa_strlist_prepend(sl, "first");
	joined = pa_strlist_to_string(sl);
	check_str("a string list keeps its order", "first second", joined);
	free(joined);
	pa_strlist_free(sl);
}

/* --- module arguments ----------------------------------------------------
 *
 * This is how the vendor options reach the HAL - "speaker_before_voice=true"
 * and the config path. A parser that drops one of them changes how the phone
 * behaves in a call.
 */
static void test_modargs(void)
{
	pa_modargs *ma = pa_modargs_new("config=/etc/x.xml speaker_before_voice=true buffers=4", NULL);
	bool flag = false;
	uint32_t n = 0;

	printf("\nmodule arguments\n");
	check("they parse", ma != NULL);
	check("arguments that are nothing at all still parse",
			pa_modargs_new(NULL, NULL) != NULL);
	check_str("a path comes back whole", "/etc/x.xml",
			pa_modargs_get_value(ma, "config", NULL));
	check_str("a missing key falls back", "fallback",
			pa_modargs_get_value(ma, "absent", "fallback"));

	check_uint("a boolean is read", 0,
			(unsigned long) pa_modargs_get_value_boolean(ma, "speaker_before_voice", &flag));
	check("and is true", flag);

	check_uint("a number is read", 0,
			(unsigned long) pa_modargs_get_value_u32(ma, "buffers", &n));
	check_uint("and has the right value", 4, n);

	check("asking for a number that is not there fails",
			pa_modargs_get_value_u32(ma, "absent", &n) < 0);
	check("and so does asking for a boolean that is not there",
			pa_modargs_get_value_boolean(ma, "absent", &flag) < 0);
	pa_modargs_free(ma);

	/* Every spelling of yes and no the ported code might meet. */
	{
		pa_modargs *b2 = pa_modargs_new("a=1 b=true c=yes d=0 e=false f=no g=maybe", NULL);
		const char *yes[] = { "a", "b", "c" };
		const char *no[] = { "d", "e", "f" };
		unsigned i;

		for (i = 0; i < 3; i++) {
			flag = false;
			pa_modargs_get_value_boolean(b2, yes[i], &flag);
			check("a spelling of yes reads as true", flag);
			flag = true;
			pa_modargs_get_value_boolean(b2, no[i], &flag);
			check("a spelling of no reads as false", !flag);
		}
		check("and something that is neither is refused",
				pa_modargs_get_value_boolean(b2, "g", &flag) < 0);
		pa_modargs_free(b2);
	}
	pa_modargs_free(NULL);
	ok("freeing nothing is harmless");
}

/* --- the process-wide registry and the core stand-in --------------------- */
static void test_shared(void)
{
	pa_core *core = pa_compat_core();
	int value = 7;

	printf("\nregistry\n");
	check("there is a core to hang things on", core != NULL);
	check("and it is always the same one", core == pa_compat_core());
	check_uint("something can be stored", 0,
			(unsigned long) pa_shared_set(core, "droid.test", &value));
	check("and found again", pa_shared_get(core, "droid.test") == &value);
	check("what was never stored is not found",
			pa_shared_get(core, "droid.absent") == NULL);
	pa_shared_remove(core, "droid.test");
	check("and after removal it is gone",
			pa_shared_get(core, "droid.test") == NULL);
}

/* --- mutex --------------------------------------------------------------- */
static void test_mutex(void)
{
	pa_mutex *m = pa_mutex_new(false, false);

	printf("\nmutex\n");
	check("it is created", m != NULL);
	pa_mutex_lock(m);
	check("a locked mutex cannot be taken again", !pa_mutex_try_lock(m));
	pa_mutex_unlock(m);
	check("and can be taken once it is free", pa_mutex_try_lock(m));
	pa_mutex_unlock(m);
	pa_mutex_free(m);

	/* The ported code sleeps between HAL retries. One millisecond is enough
	 * to prove it returns rather than to time anything. */
	{
		extern void pa_msleep(unsigned long t);
		pa_msleep(1);
	}
	ok("a millisecond of sleep comes back");
}

int main(void)
{
	test_sample();
	test_channel_map();
	test_proplist();
	test_hashmap();
	test_idxset();
	test_strings();
	test_modargs();
	test_shared();
	test_mutex();
	printf("\n  %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
