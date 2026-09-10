/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* The decisions the card makes, without a HAL anywhere near it.
 *
 * droid-device.c is included rather than linked: the interesting parts are
 * static, and the alternative - exporting them just so a test can see them -
 * would be a worse trade than this one line.
 *
 * Nothing here opens the HAL. These are the pure decisions: how a port is
 * ranked, and what the card does with the volume the graph hands it. Both have
 * been wrong in ways that were invisible until someone listened.
 */
#include <math.h>

#ifndef TEST_FIXTURE
#define TEST_FIXTURE "tests/audio-policy-fixture.xml"
#endif
#include "../src/droid-device.c"

/* droid-device.c names the node factories that live in droid-pcm.c. The test
 * never creates a node, so empty stand-ins are enough - and they keep the HAL
 * out of the test binary altogether. */
const struct spa_handle_factory droid_pcm_factory = {
	.version = SPA_VERSION_HANDLE_FACTORY, .name = "api.droid.pcm",
};
const struct spa_handle_factory droid_pcm_source_factory = {
	.version = SPA_VERSION_HANDLE_FACTORY, .name = "api.droid.pcm.source",
};

/* Same idea: in the daemon this reaches a node that holds a HAL stream. Here
 * it reports that no node is listening, which is what set_route() is written
 * to tolerate. */
int droid_node_set_route(const char *mix_port, const char *device_port)
{
	(void) mix_port;
	(void) device_port;
	return -ENOENT;
}

static int failures;
static int checks;

static void check_uint(const char *what, uint32_t want, uint32_t got)
{
	checks++;
	if (want == got) {
		printf("  \033[32mok\033[0m   %s\n", what);
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m %s: expected %u, got %u\n", what, want, got);
	}
}

static void check_float(const char *what, float want, float got)
{
	checks++;
	if (fabsf(want - got) < 0.0001f) {
		printf("  \033[32mok\033[0m   %s\n", what);
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m %s: expected %.4f, got %.4f\n", what, want, got);
	}
}

/* --- how ports are ranked -------------------------------------------------
 *
 * These numbers are not ours to choose: they are what PulseAudio's droid-card
 * assigns, and WirePlumber's route policy picks by them. The speaker has to
 * outrank the earpiece, or a phone in a pocket plays into nobody's ear.
 */
static void test_route_priority(void)
{
	printf("route priority\n");
	check_uint("the speaker outranks everything",
			300, route_priority(AUDIO_DEVICE_OUT_SPEAKER));
	check_uint("the earpiece comes second",
			200, route_priority(AUDIO_DEVICE_OUT_EARPIECE));
	check_uint("the built-in microphone ranks like the earpiece",
			200, route_priority(AUDIO_DEVICE_IN_BUILTIN_MIC));
	check_uint("the back microphone ranks below the main one",
			150, route_priority(AUDIO_DEVICE_IN_BACK_MIC));
	check_uint("the call tap ranks last - it is not a microphone",
			50, route_priority(AUDIO_DEVICE_IN_VOICE_CALL));
	check_uint("wired accessories rank below the built-in ones",
			100, route_priority(AUDIO_DEVICE_OUT_WIRED_HEADSET));
	check_uint("anything unknown ranks last",
			50, route_priority(AUDIO_DEVICE_OUT_BLUETOOTH_SCO));

	/* The one that matters: with both at 200 the winner came down to the
	 * order of the vendor's XML, and a reordering would have moved every
	 * recording to the back microphone without a word. */
	checks++;
	if (route_priority(AUDIO_DEVICE_IN_BUILTIN_MIC) >
	    route_priority(AUDIO_DEVICE_IN_BACK_MIC))
		printf("  \033[32mok\033[0m   the main microphone cannot lose to the back one\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m the back microphone can win by accident\n");
	}

	/* Losing this one is the worst case of the three: the call tap is silent
	 * outside a call, so the phone would record nothing at all. */
	checks++;
	if (route_priority(AUDIO_DEVICE_IN_BUILTIN_MIC) >
	    route_priority(AUDIO_DEVICE_IN_VOICE_CALL))
		printf("  \033[32mok\033[0m   the main microphone cannot lose to the call tap\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m the call tap can become the default microphone\n");
	}

	checks++;
	if (route_priority(AUDIO_DEVICE_OUT_SPEAKER) >
	    route_priority(AUDIO_DEVICE_OUT_EARPIECE))
		printf("  \033[32mok\033[0m   speaker beats earpiece, which is the point\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m speaker does not beat earpiece\n");
	}
}

/* --- what the card does with a volume ------------------------------------
 *
 * A node that belongs to a card takes its volume from the props of the active
 * route. Getting this wrong is quiet in the worst way: pactl showed 0 % and
 * dropped every change, while wpctl worked, so the fault looked like the
 * application's.
 */
static struct impl *fresh_impl(void)
{
	static struct impl this;
	uint32_t i, c;

	memset(&this, 0, sizeof(this));
	for (i = 0; i < N_DEVICES; i++) {
		this.volume[i] = 1.0f;
		this.mute[i] = false;
		this.n_channel_volumes[i] = 2;
		for (c = 0; c < MAX_CHANNELS; c++) {
			this.channel_volumes[i][c] = 1.0f;
			this.channel_map[i][c] = SPA_AUDIO_CHANNEL_UNKNOWN;
		}
		this.channel_map[i][0] = SPA_AUDIO_CHANNEL_FL;
		this.channel_map[i][1] = SPA_AUDIO_CHANNEL_FR;
	}
	return &this;
}

static struct spa_pod *build_props(uint8_t *buffer, size_t size,
		const float *vols, uint32_t n_vols, bool mute)
{
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, size);
	struct spa_pod_frame f;

	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
	spa_pod_builder_prop(&b, SPA_PROP_mute, 0);
	spa_pod_builder_bool(&b, mute);
	spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
	spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, n_vols, vols);
	return spa_pod_builder_pop(&b, &f);
}

static void test_route_props(void)
{
	uint8_t buffer[1024];
	struct impl *this;
	float two[2] = { 0.25f, 0.25f };
	float one[1] = { 0.5f };
	float many[4] = { 0.1f, 0.2f, 0.3f, 0.4f };

	printf("\nvolume from the active route\n");

	this = fresh_impl();
	apply_route_props(this, DEV_SINK,
			build_props(buffer, sizeof(buffer), two, 2, false));
	check_float("a stereo volume arrives on the left channel", 0.25f,
			this->channel_volumes[DEV_SINK][0]);
	check_float("and on the right", 0.25f, this->channel_volumes[DEV_SINK][1]);
	check_uint("the channel count stays at two", 2,
			this->n_channel_volumes[DEV_SINK]);

	/* This is the bug that left the right channel of the microphone at zero:
	 * a stored state from an earlier version held a single channel, and
	 * taking that at face value silenced the other one. */
	this = fresh_impl();
	apply_route_props(this, DEV_SOURCE,
			build_props(buffer, sizeof(buffer), one, 1, false));
	check_float("a single incoming value fills the first channel", 0.5f,
			this->channel_volumes[DEV_SOURCE][0]);
	check_float("and is repeated rather than leaving the second at zero",
			0.5f, this->channel_volumes[DEV_SOURCE][1]);
	check_uint("the count still says two, because the node is stereo", 2,
			this->n_channel_volumes[DEV_SOURCE]);

	this = fresh_impl();
	apply_route_props(this, DEV_SINK,
			build_props(buffer, sizeof(buffer), many, 4, false));
	check_float("more values than channels does not run off the end", 0.1f,
			this->channel_volumes[DEV_SINK][0]);

	this = fresh_impl();
	apply_route_props(this, DEV_SINK,
			build_props(buffer, sizeof(buffer), two, 2, true));
	check_uint("mute comes across", 1, this->mute[DEV_SINK] ? 1 : 0);

	/* A channel map of the right length is taken; a short one is not, because
	 * it would rename the channels - that is how front-left once became the
	 * nameless aux0. */
	this = fresh_impl();
	{
		uint8_t buf[512];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		struct spa_pod_frame f;
		uint32_t map[2] = { SPA_AUDIO_CHANNEL_FR, SPA_AUDIO_CHANNEL_FL };
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
		spa_pod_builder_prop(&b, SPA_PROP_channelMap, 0);
		spa_pod_builder_array(&b, sizeof(uint32_t), SPA_TYPE_Id, 2, map);
		apply_route_props(this, DEV_SINK, spa_pod_builder_pop(&b, &f));
	}
	check_uint("a channel map of the right length is taken",
			SPA_AUDIO_CHANNEL_FR, this->channel_map[DEV_SINK][0]);

	this = fresh_impl();
	{
		uint8_t buf[512];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		struct spa_pod_frame f;
		uint32_t map[1] = { SPA_AUDIO_CHANNEL_MONO };
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
		spa_pod_builder_prop(&b, SPA_PROP_channelMap, 0);
		spa_pod_builder_array(&b, sizeof(uint32_t), SPA_TYPE_Id, 1, map);
		apply_route_props(this, DEV_SINK, spa_pod_builder_pop(&b, &f));
	}
	check_uint("a shorter one is refused, so the channels keep their names",
			SPA_AUDIO_CHANNEL_FL, this->channel_map[DEV_SINK][0]);

	/* A device index out of range must be ignored, not written past. */
	this = fresh_impl();
	apply_route_props(this, N_DEVICES + 7,
			build_props(buffer, sizeof(buffer), two, 2, false));
	check_float("an impossible device index changes nothing", 1.0f,
			this->channel_volumes[DEV_SINK][0]);

	/* Anything that is not a Props object is not ours to interpret. */
	this = fresh_impl();
	{
		uint8_t other[256];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(other, sizeof(other));
		struct spa_pod_frame f;
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamRoute,
				SPA_PARAM_Route);
		spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_index, 0);
		spa_pod_builder_int(&b, 3);
		apply_route_props(this, DEV_SINK, spa_pod_builder_pop(&b, &f));
	}
	check_float("a pod of the wrong type is ignored", 1.0f,
			this->channel_volumes[DEV_SINK][0]);
}

/* --- what a port is called ------------------------------------------------ */
static void test_route_description(void)
{
	printf("\nport descriptions\n");

	checks++;
	if (strstr(route_description("input-voice_call", "Voice Call In"),
				"not a microphone") != NULL)
		printf("  \033[32mok\033[0m   the call tap says it is not a microphone\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m the call tap still reads like an input device\n");
	}

	/* The microphones say where they are - the holes are visible on the case
	 * - and the second one says it is not a second signal, because measuring
	 * showed both ports deliver the same capsule. A label that promised a
	 * separate top microphone would be the honest-sounding kind of wrong. */
	checks++;
	if (strstr(route_description("input-builtin_mic", "Built-In Mic"),
				"bottom") != NULL)
		printf("  \033[32mok\033[0m   the main microphone says where it is\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m the main microphone lost its position\n");
	}

	checks++;
	if (strstr(route_description("input-back_mic", "Built-In Back Mic"),
				"no separate signal") != NULL)
		printf("  \033[32mok\033[0m   the second port does not promise a second microphone\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m the second port claims a signal it does not deliver\n");
	}

	checks++;
	if (spa_streq(route_description("output-speaker", "Speaker"), "Speaker"))
		printf("  \033[32mok\033[0m   an unknown port falls back unchanged\n");
	else {
		failures++;
		printf("  \033[31mFAIL\033[0m the fallback description was altered\n");
	}
}

/* --- which route is picked when nobody chose ------------------------------
 *
 * The priorities are only half the story: this is the function that turns them
 * into a decision, and it had no test at all while a commit message claimed
 * the main microphone could not lose. It can now be asked directly.
 */
static void add_route(struct impl *this, const char *pa_name, uint32_t device,
		uint32_t priority, uint32_t available)
{
	struct route *r = &this->routes[this->n_routes++];

	/* pa_name is a pointer, not a buffer - in the real thing it points into
	 * the parsed HAL configuration. String literals outlive this test. */
	r->pa_name = pa_name;
	r->device = device;
	r->priority = priority;
	r->available = available;
}

static const char *picked(struct impl *this, uint32_t device)
{
	uint32_t idx = default_route(this, device);
	return idx == SPA_ID_INVALID ? "(none)" : this->routes[idx].pa_name;
}

static void check_str(const char *what, const char *want, const char *got)
{
	checks++;
	if (spa_streq(want, got)) {
		printf("  \033[32mok\033[0m   %s\n", what);
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m %s: expected %s, got %s\n", what, want, got);
	}
}

static void test_default_route(void)
{
	struct impl *this;

	printf("\nwhich route is picked when nobody chose\n");

	/* The real thing, in the order the vendor's XML lists them. */
	this = fresh_impl();
	add_route(this, "input-builtin_mic", DEV_SOURCE, 200, SPA_PARAM_AVAILABILITY_yes);
	add_route(this, "input-back_mic", DEV_SOURCE, 150, SPA_PARAM_AVAILABILITY_yes);
	add_route(this, "input-wired_headset", DEV_SOURCE, 100, SPA_PARAM_AVAILABILITY_no);
	add_route(this, "input-voice_call", DEV_SOURCE, 50, SPA_PARAM_AVAILABILITY_yes);
	check_str("the main microphone wins", "input-builtin_mic",
			picked(this, DEV_SOURCE));

	/* The same set in the reverse order. This is the case that matters: with
	 * everything at 200, as PulseAudio ranks them, the winner used to be
	 * whichever the XML happened to list first, and a vendor reordering would
	 * have moved every recording to the call tap. */
	this = fresh_impl();
	add_route(this, "input-voice_call", DEV_SOURCE, 50, SPA_PARAM_AVAILABILITY_yes);
	add_route(this, "input-back_mic", DEV_SOURCE, 150, SPA_PARAM_AVAILABILITY_yes);
	add_route(this, "input-builtin_mic", DEV_SOURCE, 200, SPA_PARAM_AVAILABILITY_yes);
	check_str("and still wins when it is listed last", "input-builtin_mic",
			picked(this, DEV_SOURCE));

	/* Wired accessories are reported unavailable - there is no jack detection
	 * on this device - and must not be preselected even when they outrank
	 * what is left. */
	this = fresh_impl();
	add_route(this, "output-wired_headset", DEV_SINK, 900, SPA_PARAM_AVAILABILITY_no);
	add_route(this, "output-speaker", DEV_SINK, 300, SPA_PARAM_AVAILABILITY_yes);
	check_str("an unavailable route is not picked, however high it ranks",
			"output-speaker", picked(this, DEV_SINK));

	/* Bluetooth reports "unknown": selectable, never automatic. */
	this = fresh_impl();
	add_route(this, "output-bluetooth_sco", DEV_SINK, 50,
			SPA_PARAM_AVAILABILITY_unknown);
	add_route(this, "output-speaker", DEV_SINK, 300, SPA_PARAM_AVAILABILITY_yes);
	check_str("nor is one whose availability is unknown", "output-speaker",
			picked(this, DEV_SINK));

	/* Routes belong to one direction. Picking an output for the microphone
	 * would be a fine way to lose an afternoon. */
	this = fresh_impl();
	add_route(this, "output-speaker", DEV_SINK, 300, SPA_PARAM_AVAILABILITY_yes);
	check_str("a direction with no routes picks nothing", "(none)",
			picked(this, DEV_SOURCE));

	this = fresh_impl();
	check_str("no routes at all picks nothing", "(none)", picked(this, DEV_SINK));
}

/* --- what the card actually publishes about a route -----------------------
 *
 * build_route_body() produces the Route param the rest of the system reads.
 * Everything that went wrong with the volume this morning went wrong here:
 * a route without volume props makes pipewire-pulse report 0 % and drop every
 * change, while wpctl keeps working, so the fault looks like the caller's.
 */
static void test_route_body(void)
{
	uint8_t buffer[4096];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod_frame f;
	struct spa_pod *param;
	struct impl *this;
	struct spa_pod_object *obj;
	struct spa_pod_prop *prop;
	bool saw_volume = false, saw_mute = false, saw_channels = false;
	bool saw_map = false, saw_params = false, saw_name = false;

	printf("\nwhat a route tells the rest of the system\n");

	this = fresh_impl();
	add_route(this, "output-speaker", DEV_SINK, 300, SPA_PARAM_AVAILABILITY_yes);
	snprintf(this->routes[0].description, sizeof(this->routes[0].description),
			"%s", "Speaker");
	this->channel_volumes[DEV_SINK][0] = 0.5f;

	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	build_route_body(this, &b, &this->routes[0], 0, true);
	param = spa_pod_builder_pop(&b, &f);

	obj = (struct spa_pod_object *) param;
	SPA_POD_OBJECT_FOREACH(obj, prop) {
		if (prop->key == SPA_PARAM_ROUTE_name)
			saw_name = true;
		if (prop->key != SPA_PARAM_ROUTE_props)
			continue;
		{
			struct spa_pod_object *po = (struct spa_pod_object *) &prop->value;
			struct spa_pod_prop *pp;
			SPA_POD_OBJECT_FOREACH(po, pp) {
				switch (pp->key) {
				case SPA_PROP_volume:        saw_volume = true; break;
				case SPA_PROP_mute:          saw_mute = true; break;
				case SPA_PROP_channelVolumes: saw_channels = true; break;
				case SPA_PROP_channelMap:    saw_map = true; break;
				case SPA_PROP_params:        saw_params = true; break;
				default: break;
				}
			}
		}
	}

	check_uint("the route carries its name", 1, saw_name ? 1 : 0);
	check_uint("and a volume, without which PulseAudio clients see 0 %",
			1, saw_volume ? 1 : 0);
	check_uint("and a mute", 1, saw_mute ? 1 : 0);
	check_uint("and per-channel volumes", 1, saw_channels ? 1 : 0);
	check_uint("and the channel map that goes with them - without it "
			"front-left becomes aux0", 1, saw_map ? 1 : 0);
	check_uint("and the props pair that carries the route across to the node",
			1, saw_params ? 1 : 0);

	/* Without the device index the route is not a route anyone can set. */
	b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
	build_route_body(this, &b, &this->routes[0], 0, false);
	param = spa_pod_builder_pop(&b, &f);
	obj = (struct spa_pod_object *) param;
	saw_params = false;
	SPA_POD_OBJECT_FOREACH(obj, prop)
		if (prop->key == SPA_PARAM_ROUTE_props)
			saw_params = true;
	check_uint("an enumerated route carries no props - only the active one does",
			0, saw_params ? 1 : 0);
}

/* The card's own description is repeated behind every port a desktop lists, so
 * it has to be a word the owner of the phone can use - and it cannot be empty,
 * because WirePlumber then creates no card at all and there is no sound. */
static void test_device_description(void)
{
	const char *desc;

	printf("\nwhat the card calls itself\n");

	{
		const struct spa_interface_info *iinfo = NULL;
		uint32_t index = 0;
		int res = droid_device_factory.enum_interface_info(
				&droid_device_factory, &iinfo, &index);
		check_uint("the plugin announces one interface: a device", 1,
				(res == 1 && iinfo != NULL &&
				 spa_streq(iinfo->type, SPA_TYPE_INTERFACE_Device)) ? 1 : 0);
		res = droid_device_factory.enum_interface_info(
				&droid_device_factory, &iinfo, &index);
		check_uint("and no more than that", 0, res);
	}

	desc = device_description();
	check_uint("the card has a description at all", 1,
			(desc != NULL && desc[0] != '\0') ? 1 : 0);

	checks++;
	if (strstr(desc, "HAL") == NULL) {
		printf("  \033[32mok\033[0m   and it is not jargon - no \"HAL\" behind every port\n");
	} else {
		failures++;
		printf("  \033[31mFAIL\033[0m the card description is back to jargon\n");
	}
}

/* --- the card as PipeWire sees it -----------------------------------------
 *
 * Everything above works on a hand-built struct impl. This part starts the
 * device the way the daemon does - init, listen, enumerate, set - against a
 * fixture configuration instead of the phone's own. No HAL is opened: the
 * device parses the XML and hands out parameters, and it is the node that
 * would touch hardware.
 */
struct listener_counts {
	uint32_t info;
	uint32_t objects;
	uint32_t removed;
	uint32_t results;
	uint32_t answers;
};

/* enum_params answers through this, not through its return value - the device
 * builds each pod and emits it, one result per parameter. */
static void on_result(void *data, int seq, int res, uint32_t type,
		const void *result)
{
	struct listener_counts *c = data;
	(void) seq; (void) res; (void) type;
	c->answers++;
	/* sync answers with an empty result - it is a marker, not a parameter. */
	if (result != NULL)
		c->results++;
}

static void on_device_info(void *data, const struct spa_device_info *info)
{
	struct listener_counts *c = data;
	(void) info;
	c->info++;
}

static void on_object_info(void *data, uint32_t id,
		const struct spa_device_object_info *info)
{
	struct listener_counts *c = data;
	if (info == NULL)
		c->removed++;
	else
		c->objects++;
	(void) id;
}

static const struct spa_device_events device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.info = on_device_info,
	.result = on_result,
	.object_info = on_object_info,
};

static struct spa_handle *start_device(const char *config)
{
	struct spa_dict_item items[2];
	struct spa_dict info;
	struct spa_handle *handle;
	size_t size = droid_device_factory.get_size ?
		droid_device_factory.get_size(&droid_device_factory, NULL) :
		sizeof(struct impl);

	handle = calloc(1, size);
	items[0] = SPA_DICT_ITEM_INIT("droid.config", config);
	info = SPA_DICT_INIT(items, 1);

	if (droid_device_factory.init(&droid_device_factory, handle, &info,
				NULL, 0) < 0) {
		free(handle);
		return NULL;
	}
	return handle;
}

static uint32_t count_params(struct spa_device *dev,
		struct listener_counts *counts, uint32_t id)
{
	counts->results = 0;
	counts->answers = 0;
	spa_device_enum_params(dev, 0, id, 0, UINT32_MAX, NULL);
	return counts->results;
}

/* Build the Profile param the card would publish and read its save flag back.
 * build_profile() is where the decision lives; asking the card's own field
 * would test the wrong thing, since the field is set and the flag is not. */
static bool profile_save_flag(struct impl *this, uint32_t index)
{
	uint8_t buffer[2048];
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
	struct spa_pod *param = NULL;
	uint32_t saved = this->profile;
	bool save = false;

	this->profile = index;
	if (build_profile(this, &b, SPA_PARAM_Profile, index, true, &param) == 1 &&
			param != NULL)
		spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, NULL,
				SPA_PARAM_PROFILE_save, SPA_POD_OPT_Bool(&save));
	this->profile = saved;
	return save;
}

static void test_device_lifecycle(const char *fixture)
{
	struct spa_handle *handle;
	struct spa_device *dev = NULL;
	struct listener_counts counts = { 0, 0, 0 };
	struct spa_hook listener = { 0 };
	struct impl *this;

	printf("\nthe card, started the way the daemon starts it\n");

	handle = start_device(fixture);
	checks++;
	if (handle == NULL) {
		failures++;
		printf("  \033[31mFAIL\033[0m the device would not initialise from the fixture\n");
		return;
	}
	printf("  \033[32mok\033[0m   it initialises from a configuration file\n");

	spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device, (void **) &dev);
	check_uint("and hands out a device interface", 1, dev != NULL ? 1 : 0);
	if (dev == NULL)
		goto out;

	this = (struct impl *) handle;

	/* The fixture has speaker, earpiece and a wired output, two microphones
	 * and the call tap - plus the two Bluetooth routes the card adds itself,
	 * because the vendor's file leaves them out. */
	check_uint("it finds the ports in the file and adds Bluetooth itself",
			8, this->n_routes);
	check_str("and picks the speaker for output", "output-speaker",
			picked(this, DEV_SINK));
	check_str("and the main microphone for input", "input-builtin_mic",
			picked(this, DEV_SOURCE));

	spa_device_add_listener(dev, &listener, &device_events, &counts);
	check_uint("a listener is told about the card", 1, counts.info > 0 ? 1 : 0);
	check_uint("and about the nodes it should create", 4, counts.objects);

	/* off, default, voicecall, communication - callaudiod looks for the
	 * middle two by name, so the count is not incidental. */
	check_uint("it offers four profiles", 4,
			count_params(dev, &counts, SPA_PARAM_EnumProfile));
	check_uint("and one route per port it found", 8,
			count_params(dev, &counts, SPA_PARAM_EnumRoute));
	check_uint("and reports the two routes that are active", 2,
			count_params(dev, &counts, SPA_PARAM_Route));
	check_uint("and the profile it is in", 1,
			count_params(dev, &counts, SPA_PARAM_Profile));

	/* --- setting things, the way pactl and WirePlumber do -------------- */

	{
		uint8_t buffer[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		struct spa_pod *param;
		uint32_t i, call = SPA_ID_INVALID, earpiece = SPA_ID_INVALID;

		/* The call profile by name, because that is how callaudiod finds
		 * it - and the name is not ours to choose. */
		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(PROFILE_VOICECALL),
				SPA_PARAM_PROFILE_save, SPA_POD_Bool(true));
		check_uint("the call profile can be set", 0,
				spa_device_set_param(dev, SPA_PARAM_Profile, 0, param) == 0 ? 0 : 1);
		check_uint("and the card is in it", PROFILE_VOICECALL, this->profile);

		/* What matters is not what the card remembers internally but what
		 * it tells WirePlumber, because that is what ends up in the stored
		 * profile state. The call profile must report save=false however
		 * insistently it was set - a phone that boots into call mode with
		 * nobody on the line is the failure this prevents. */
		check_uint("but it never reports the call profile as a choice to keep",
				0, profile_save_flag(this, PROFILE_VOICECALL) ? 1 : 0);
		check_uint("nor the communication profile", 0,
				profile_save_flag(this, PROFILE_COMMUNICATION) ? 1 : 0);

		b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamProfile, SPA_PARAM_Profile,
				SPA_PARAM_PROFILE_index, SPA_POD_Int(PROFILE_DEFAULT),
				SPA_PARAM_PROFILE_save, SPA_POD_Bool(true));
		spa_device_set_param(dev, SPA_PARAM_Profile, 0, param);
		check_uint("an ordinary profile is reported as one to keep", 1,
				profile_save_flag(this, PROFILE_DEFAULT) ? 1 : 0);

		for (i = 0; i < this->n_routes; i++) {
			if (spa_streq(this->routes[i].pa_name, "output-earpiece"))
				earpiece = i;
			if (spa_streq(this->routes[i].pa_name, "input-voice_call"))
				call = i;
		}

		b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(earpiece),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(DEV_SINK));
		spa_device_set_param(dev, SPA_PARAM_Route, 0, param);
		check_str("a route can be chosen", "output-earpiece",
				this->routes[this->active[DEV_SINK]].pa_name);

		/* A route belongs to one direction. Setting an output as the
		 * microphone must be refused, not obeyed. */
		b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(earpiece),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(DEV_SOURCE));
		check_uint("but not for the wrong direction", 1,
				spa_device_set_param(dev, SPA_PARAM_Route, 0, param) < 0 ? 1 : 0);
		check_str("and the microphone is untouched", "input-builtin_mic",
				this->routes[this->active[DEV_SOURCE]].pa_name);
		(void) call;

		/* An index past the end must not be followed anywhere. */
		b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
		param = spa_pod_builder_add_object(&b,
				SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route,
				SPA_PARAM_ROUTE_index, SPA_POD_Int(this->n_routes + 5),
				SPA_PARAM_ROUTE_device, SPA_POD_Int(DEV_SINK));
		check_uint("an index past the end is refused", 1,
				spa_device_set_param(dev, SPA_PARAM_Route, 0, param) < 0 ? 1 : 0);

		/* Volume arrives on the route, which is where PulseAudio clients
		 * read and write it. */
		{
			float vols[2] = { 0.3f, 0.3f };
			/* One frame per object. Reusing a single one nests the pops
			 * wrongly and the builder then writes past its buffer - which
			 * showed up as the whole test hanging somewhere else entirely. */
			struct spa_pod_frame f_route, f_props;

			b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
			spa_pod_builder_push_object(&b, &f_route,
					SPA_TYPE_OBJECT_ParamRoute, SPA_PARAM_Route);
			spa_pod_builder_add(&b,
					SPA_PARAM_ROUTE_index, SPA_POD_Int(earpiece),
					SPA_PARAM_ROUTE_device, SPA_POD_Int(DEV_SINK), 0);
			spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
			spa_pod_builder_push_object(&b, &f_props,
					SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
			spa_pod_builder_prop(&b, SPA_PROP_channelVolumes, 0);
			spa_pod_builder_array(&b, sizeof(float), SPA_TYPE_Float, 2, vols);
			spa_pod_builder_pop(&b, &f_props);
			param = spa_pod_builder_pop(&b, &f_route);
			spa_device_set_param(dev, SPA_PARAM_Route, 0, param);
		}
		check_float("a volume set on the route arrives", 0.3f,
				this->channel_volumes[DEV_SINK][0]);
	}

	/* sync answers with a result the caller can wait on - WirePlumber uses it
	 * to know the card has finished announcing itself. */
	counts.answers = counts.results = 0;
	spa_device_sync(dev, 42);
	check_uint("sync answers, with an empty result", 1,
			(counts.answers == 1 && counts.results == 0) ? 1 : 0);

	spa_hook_remove(&listener);
out:
	spa_handle_clear(handle);
	free(handle);
}

int main(void)
{
	test_route_description();
	test_device_description();
	test_route_body();
	test_device_lifecycle(TEST_FIXTURE);
	test_default_route();
	test_route_priority();
	test_route_props();
	printf("\n  %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
