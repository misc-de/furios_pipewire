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
	check_uint("wired accessories rank below the built-in ones",
			100, route_priority(AUDIO_DEVICE_OUT_WIRED_HEADSET));
	check_uint("anything unknown ranks last",
			50, route_priority(AUDIO_DEVICE_OUT_BLUETOOTH_SCO));

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

int main(void)
{
	test_route_priority();
	test_route_props();
	printf("\n  %d checks, %d failed\n", checks, failures);
	return failures == 0 ? 0 : 1;
}
