/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* SPA device for the Android audio HAL.
 *
 * The device is the card: it reports profiles and routes and creates the two
 * nodes (playback/capture). The route names are deliberately exactly the ones
 * PulseAudio's droid-card uses - "output-earpiece", "output-speaker",
 * "input-builtin_mic" - because callaudiod looks for precisely those names to
 * switch between earpiece and speaker during a call.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/utils/names.h>
#include <spa/utils/hook.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>
#include <spa/utils/result.h>
#include <spa/node/node.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>
#include <spa/param/param.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>
#include <spa/pod/filter.h>
#include <spa/pod/parser.h>

#include <hardware/audio.h>
#include "droid/droid-config.h"
#include "droid/conversion.h"
#include "droid/sllist.h"

#define NAME "droid-device"

/* Exactly the identifier PulseAudio's droid module sets
 * (PROP_DROID_API_STRING). callaudiod recognises an Android HAL card by
 * nothing else and only then switches between "default" and "voicecall" -
 * with "droid" it falls back to the UCM path and does nothing. */
#define DROID_API_NAME  "droid-hal"

#define MAX_ROUTES  16
#define MAX_CHANNELS 8
#define DEV_SINK    0
#define DEV_SOURCE  1
#define N_DEVICES   2

/* Extra nodes for the HAL's VoIP path. They sit on their own mix ports
 * (voip_rx/voip_tx) and deliberately get NO card.profile.device: routing goes
 * through the primary stream, and the route policy should leave them
 * alone. */
#define OBJ_VOIP_SINK   2
#define OBJ_VOIP_SOURCE 3

#define PROFILE_OFF           0
#define PROFILE_DEFAULT       1
#define PROFILE_VOICECALL     2
#define PROFILE_COMMUNICATION 3

/* The name is not ours to choose: callaudiod looks for exactly this name in
 * the card profiles ("card has voice profile, using it"). */
#define VOICECALL_NAME  "voicecall"

/* from droid-pcm.c. The second argument is the ROUTE name ("output-speaker"),
 * not the HAL's own port name ("Speaker") - the node resolves it through
 * pa_droid_output_port_name(), which only ever yields route names. Handing it
 * a port name looks right and matches nothing. */
/* Only reaches a node that lives in THIS process. WirePlumber loads the card
 * into its own, so in the normal setup this finds nothing and returns -ENOENT
 * - which is why every route that matters travels as props on the route
 * instead. Kept for a host that does load both together. */
int droid_node_set_route(const char *mix_port, const char *route);

struct route {
	dm_config_port *port;      /* device port from the HAL configuration */
	const char *pa_name;       /* "output-speaker" and so on */
	char description[64];
	enum spa_direction dir;
	uint32_t device;           /* DEV_SINK or DEV_SOURCE */
	uint32_t priority;
	uint32_t available;        /* SPA_PARAM_AVAILABILITY_* */
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	bool diag;                 /* verbose diagnostics (SPA_DROID_DIAG=1) */

	struct spa_log *log;
	struct spa_hook_list hooks;

	struct spa_device_info info;
	struct spa_param_info params[4];

	dm_config_device *config;
	dm_config_module *module;

	struct route routes[MAX_ROUTES];
	uint32_t n_routes;
	uint32_t active[N_DEVICES];   /* index into routes[], SPA_ID_INVALID = none */

	uint32_t profile;
	bool profile_save;         /* was this a deliberate choice? */
	bool nodes_emitted;

	/* Volume and mute per direction, as the active route reports them.
	 *
	 * A node that belongs to a card gets its volume from the props of the
	 * active route - that is where pipewire-pulse reads it, and a card
	 * whose routes carry none reports 0 % to every PulseAudio client and
	 * silently drops what they set. The attenuation itself stays in the
	 * graph; these values are the card's side of it. */
	float volume[N_DEVICES];
	float channel_volumes[N_DEVICES][MAX_CHANNELS];
	uint32_t channel_map[N_DEVICES][MAX_CHANNELS];
	uint32_t n_channel_volumes[N_DEVICES];
	bool mute[N_DEVICES];
};

/* As in the node: info by default (invisible below the standard log level),
 * raised to warn with SPA_DROID_DIAG=1. */
#define DIAG(this, fmt, ...)						\
	do {								\
		if ((this)->diag)					\
			spa_log_warn((this)->log, NAME " " fmt, ##__VA_ARGS__); \
		else							\
			spa_log_info((this)->log, NAME " " fmt, ##__VA_ARGS__); \
	} while (0)

static const char *default_config_file(void)
{
	return "/android/vendor/etc/audio_policy_configuration.xml";
}

static const char *mix_port_of(uint32_t device)
{
	return device == DEV_SINK ? "primary output" : "primary input";
}

/* ------------------------------------------------------------- routes */

/* How important is a port? Speaker and earpiece are what a call is about;
 * wired accessories beat them when plugged in. */
static uint32_t route_priority(audio_devices_t type)
{
	/* The same values PulseAudio's droid-card assigns. */
	switch (type) {
	case AUDIO_DEVICE_OUT_SPEAKER:
		return 300;
	case AUDIO_DEVICE_OUT_EARPIECE:
	case AUDIO_DEVICE_IN_BUILTIN_MIC:
		return 200;
	/* Never by accident. This is not a microphone at all - it is the tap on
	 * the call itself, and outside a call it delivers digital silence
	 * (measured: 48000 samples, one distinct value, and that value zero).
	 * PulseAudio's droid-card ranks it 200, level with the real microphone,
	 * so which of the two becomes the default comes down to the order of the
	 * vendor's XML. Losing that coin toss would leave the phone recording
	 * nothing at all, and the cause is nowhere near the symptom. */
	case AUDIO_DEVICE_IN_VOICE_CALL:
		return 50;
	/* Below the main microphone on purpose. PulseAudio's droid-card gives
	 * both 200, and then which one is picked comes down to the order they
	 * happen to appear in the vendor's XML - the front one only wins because
	 * it is listed first. A vendor update reordering that file would move
	 * every recording to the back microphone, silently. Measured with the
	 * same tone from the speaker, the two are within 5 % of each other
	 * anyway (RMS 87.5 against 83.7), so nothing is lost by making the
	 * choice deterministic. */
	case AUDIO_DEVICE_IN_BACK_MIC:
		return 150;
	case AUDIO_DEVICE_OUT_WIRED_HEADSET:
	case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
	case AUDIO_DEVICE_IN_WIRED_HEADSET:
		return 100;
	default:
		return 50;
	}
}

/* On this device Bluetooth is entirely commented out of the audio_policy XML
 * - a block of roughly 3900 characters takes ten device ports with it,
 * including every BT SCO and A2DP entry. The HAL does not need that file
 * though: when routing it only gets the device type, and for BT SCO types
 * pa_droid_stream_set_route additionally sends BT_SCO=on to the HAL - exactly
 * the Android mechanism that puts the voice path onto the Bluetooth PCM line.
 * So we build the ports ourselves.
 *
 * This device's controller does not carry SCO over HCI - measured on a link
 * that really stood: 3987 packets sent, zero received. The data goes over
 * MediaTek's own link to the application processor (ALSA device 55, BTCVSD),
 * where the HAL runs the codec in software. That makes this the only path for
 * calls over a headset, and it does work: see the README. */
static dm_config_port synthetic_ports[2];

static void add_synthetic_bt_routes(struct impl *this)
{
	static const struct {
		const char *name;
		const char *pa_name;
		audio_devices_t type;
		dm_config_role_t role;
		uint32_t device;
	} defs[] = {
		{ "BT SCO",             "output-bluetooth_sco",        AUDIO_DEVICE_OUT_BLUETOOTH_SCO,
		  DM_CONFIG_ROLE_SINK,   DEV_SINK },
		{ "BT SCO Headset Mic", "input-bluetooth_sco_headset", AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET,
		  DM_CONFIG_ROLE_SOURCE, DEV_SOURCE },
	};
	uint32_t i;

	for (i = 0; i < SPA_N_ELEMENTS(defs) && this->n_routes < MAX_ROUTES; i++) {
		dm_config_port *p = &synthetic_ports[i];
		struct route *r = &this->routes[this->n_routes++];

		p->module = this->module;
		p->port_type = DM_CONFIG_TYPE_DEVICE_PORT;
		p->name = (char *) defs[i].name;
		p->role = defs[i].role;
		p->type = defs[i].type;
		p->address = (char *) "";

		r->port = p;
		r->pa_name = defs[i].pa_name;
		r->dir = defs[i].role == DM_CONFIG_ROLE_SINK
			? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
		r->device = defs[i].device;
		r->priority = 50;
		/* The card does not know whether a headset is actually connected -
		 * that is the Bluetooth stack's business. Hence "unknown":
		 * selectable, but nothing picks it on its own. */
		r->available = SPA_PARAM_AVAILABILITY_unknown;
		snprintf(r->description, sizeof(r->description), "Bluetooth (%s)",
				defs[i].role == DM_CONFIG_ROLE_SINK ? "Handsfree" : "Microphone");
	}
}

/* What to call a port, where the vendor's own label misleads.
 *
 * The descriptions otherwise come straight from the tagName in Android's
 * audio_policy XML, which is the right default: it is the manufacturer's own
 * word for their own hardware, and inventing better ones is how a description
 * ends up claiming something nobody verified.
 *
 * The exception is a label that is not just terse but wrong about what the
 * thing is. "Voice Call In" reads like a microphone and is not one - it is the
 * tap on the call audio path, silent outside a call. Someone picking it as
 * their recording input gets nothing and has no way to guess why. */
/* What the card calls itself.
 *
 * Desktops list an input as "<port> - <card>", so this word is repeated behind
 * every microphone and speaker the phone has. "Android HAL" told the owner of
 * the phone nothing they could use. It cannot simply be left out either: with
 * an empty description WirePlumber creates no card at all, so the shorter
 * label would cost all sound. */
static const char *device_description(void)
{
	return "Phone";
}

static const char *route_description(const char *pa_name, const char *fallback)
{
	static const struct {
		const char *pa_name;
		const char *description;
	} overrides[] = {
		{ "input-voice_call", "Voice Call Tap (not a microphone)" },
		/* The phone has two microphone holes, one at each end - that much is
		 * visible on the case. Android's own labels ("Built-In Mic",
		 * "Built-In Back Mic") say which port is primary but nothing about
		 * where, so the position is worth adding.
		 *
		 * What is not worth pretending is that selecting the second one gets
		 * you the second capsule. It does not, on this HAL: with the
		 * bottom-firing speaker playing, both ports measured 2876-3011 RMS
		 * across three runs each - under four percent apart, where a
		 * microphone at the far end of the phone would be several decibels
		 * down. The HAL picks its capsule by audio source and mode, not by
		 * the device we route to. So the label says where the hole is and
		 * that the signal is the same one. */
		{ "input-builtin_mic", "Built-In Mic (bottom)" },
		{ "input-back_mic",    "Built-In Mic (top, no separate signal)" },
	};
	size_t i;

	for (i = 0; i < SPA_N_ELEMENTS(overrides); i++)
		if (spa_streq(pa_name, overrides[i].pa_name))
			return overrides[i].description;
	return fallback;
}

/* The PulseAudio name for a device port, or nothing.
 *
 * Without such a name the route is worthless to callaudiod and every other
 * tool, because they all match on these names - so a port that has none is
 * left out rather than published as something nothing can address.
 *
 * On this device every type the configuration parser understands happens to
 * have a name, so the refusal never fires in practice. It stays because the
 * next vendor's file is not this one's, and it is a separate function so a
 * test can ask it directly rather than hunting for a port type that has no
 * name and can still be parsed - there is none. */
static bool route_pa_name(audio_devices_t type, bool output, const char **name)
{
	return output ? pa_droid_output_port_name(type, name)
		      : pa_droid_input_port_name(type, name);
}

static void collect_routes(struct impl *this)
{
	dm_config_port *port;
	void *state;

	for (port = dm_list_first_data(this->module->device_ports, &state); port;
	     port = dm_list_next_data(this->module->device_ports, &state)) {
		struct route *r;
		const char *pa_name = NULL;
		bool output = port->role == DM_CONFIG_ROLE_SINK;

		if (this->n_routes >= MAX_ROUTES)
			break;

		if (!route_pa_name(port->type, output, &pa_name))
			continue;

		r = &this->routes[this->n_routes++];
		r->port = port;
		r->pa_name = pa_name;
		r->dir = output ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
		r->device = output ? DEV_SINK : DEV_SOURCE;
		r->priority = route_priority(port->type);
		/* Wired accessories count as not connected. That is not arbitrary
		 * but what PulseAudio reports on this device as well - there is no
		 * jack detection here (/sys/class/extcon only lists USB). And it
		 * matters: with droid cards callaudiod grabs the headset as soon as
		 * it is not marked unavailable - during a call the audio would then
		 * end up nowhere instead of on the earpiece. */
		switch (port->type) {
		case AUDIO_DEVICE_OUT_WIRED_HEADSET:
		case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
		case AUDIO_DEVICE_IN_WIRED_HEADSET:
			r->available = SPA_PARAM_AVAILABILITY_no;
			break;
		default:
			r->available = SPA_PARAM_AVAILABILITY_yes;
			break;
		}
		snprintf(r->description, sizeof(r->description), "%s",
				route_description(r->pa_name, port->name));
	}

	add_synthetic_bt_routes(this);
}

static uint32_t default_route(struct impl *this, uint32_t device)
{
	uint32_t i, best = SPA_ID_INVALID, best_prio = 0;

	for (i = 0; i < this->n_routes; i++) {
		/* Do not preselect wired accessories blindly - we do not (yet)
		 * know whether anything is plugged in. */
		if (this->routes[i].device != device ||
		    this->routes[i].available != SPA_PARAM_AVAILABILITY_yes)
			continue;
		if (best == SPA_ID_INVALID || this->routes[i].priority > best_prio) {
			best = i;
			best_prio = this->routes[i].priority;
		}
	}
	return best;
}

static int build_profile(struct impl *this, struct spa_pod_builder *b,
		uint32_t id, uint32_t index, bool current, struct spa_pod **param)
{
	struct spa_pod_frame f[4];
	const char *name, *desc;
	uint32_t prio;

	switch (index) {
	case PROFILE_OFF:
		name = "off"; desc = "Off"; prio = 0;
		break;
	case PROFILE_DEFAULT:
		name = "default"; desc = "Playback and Capture"; prio = 100;
		break;
	case PROFILE_VOICECALL:
		/* Lower priority than default - callaudiod selects this profile
		 * during a call, the route policy never picks it by itself. */
		name = VOICECALL_NAME; desc = "Voice Call"; prio = 50;
		break;
	case PROFILE_COMMUNICATION:
		/* AUDIO_MODE_IN_COMMUNICATION: this is what makes the HAL turn on
		 * its echo cancellation and noise reduction. Meant for VoIP - named
		 * as in PulseAudio's droid-card so existing tools find it. Nothing
		 * selects it on its own. */
		name = "communication"; desc = "VoIP Call"; prio = 40;
		break;
	default:
		return 0;
	}

	spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_ParamProfile, id);
	spa_pod_builder_add(b,
		SPA_PARAM_PROFILE_index,       SPA_POD_Int(index),
		SPA_PARAM_PROFILE_name,        SPA_POD_String(name),
		SPA_PARAM_PROFILE_description, SPA_POD_String(desc),
		SPA_PARAM_PROFILE_priority,    SPA_POD_Int(prio),
		SPA_PARAM_PROFILE_available,   SPA_POD_Id(SPA_PARAM_AVAILABILITY_yes),
		0);

	if (index != PROFILE_OFF) {
		spa_pod_builder_prop(b, SPA_PARAM_PROFILE_classes, 0);
		spa_pod_builder_push_struct(b, &f[1]);
		spa_pod_builder_int(b, 2);

		spa_pod_builder_push_struct(b, &f[2]);
		spa_pod_builder_string(b, "Audio/Sink");
		spa_pod_builder_int(b, 1);
		spa_pod_builder_string(b, "card.profile.devices");
		spa_pod_builder_push_array(b, &f[3]);
		spa_pod_builder_int(b, DEV_SINK);
		spa_pod_builder_pop(b, &f[3]);
		spa_pod_builder_pop(b, &f[2]);

		spa_pod_builder_push_struct(b, &f[2]);
		spa_pod_builder_string(b, "Audio/Source");
		spa_pod_builder_int(b, 1);
		spa_pod_builder_string(b, "card.profile.devices");
		spa_pod_builder_push_array(b, &f[3]);
		spa_pod_builder_int(b, DEV_SOURCE);
		spa_pod_builder_pop(b, &f[3]);
		spa_pod_builder_pop(b, &f[2]);

		spa_pod_builder_pop(b, &f[1]);
	}

	/* Without the save flag WirePlumber treats the selection as an
	 * incidental change and does not put it into the profile state.
	 *
	 * The call profile is the exception: it is NEVER saved. Otherwise
	 * WirePlumber would remember a profile that gets restored on the next
	 * start - the phone would boot in call mode with nobody on the
	 * line. */
	if (current)
		spa_pod_builder_add(b, SPA_PARAM_PROFILE_save,
				SPA_POD_Bool(index != PROFILE_VOICECALL &&
					     index != PROFILE_COMMUNICATION &&
					     this->profile_save), 0);

	*param = spa_pod_builder_pop(b, &f[0]);
	return 1;
}

static void build_route_body(struct impl *this, struct spa_pod_builder *b,
		struct route *r, uint32_t index, bool with_device)
{
	struct spa_pod_frame f;

	spa_pod_builder_add(b,
		SPA_PARAM_ROUTE_index,       SPA_POD_Int(index),
		SPA_PARAM_ROUTE_direction,   SPA_POD_Id(r->dir),
		SPA_PARAM_ROUTE_name,        SPA_POD_String(r->pa_name),
		SPA_PARAM_ROUTE_description, SPA_POD_String(r->description),
		SPA_PARAM_ROUTE_priority,    SPA_POD_Int(r->priority),
		SPA_PARAM_ROUTE_available,   SPA_POD_Id(r->available),
		0);

	if (with_device) {
		struct spa_pod_frame pf, sf;

		spa_pod_builder_add(b, SPA_PARAM_ROUTE_device, SPA_POD_Int(r->device), 0);

		/* The route carries its name along as a prop, for anything that
		 * reads the card's parameters. It is NOT how the name reaches the
		 * node, though - measured: the node receives droid.route and nothing
		 * else from this struct. droid.lua reads the active route and sends
		 * the name on itself (setNodeProp), which is the only road across:
		 * the card lives in WirePlumber's process and the nodes in
		 * PipeWire's. Anything else that has to reach a node goes the same
		 * way, through droid.lua. */
		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_props, 0);
		spa_pod_builder_push_object(b, &pf, SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);

		spa_pod_builder_prop(b, SPA_PROP_volume, 0);
		spa_pod_builder_float(b, this->volume[r->device]);
		spa_pod_builder_prop(b, SPA_PROP_mute, 0);
		spa_pod_builder_bool(b, this->mute[r->device]);
		/* The positions have to travel with the volumes, and before them:
		 * per-channel volumes without a map turn front-left/front-right into
		 * the nameless aux0/aux1 on the PulseAudio side. */
		spa_pod_builder_prop(b, SPA_PROP_channelMap, 0);
		spa_pod_builder_array(b, sizeof(uint32_t), SPA_TYPE_Id,
				this->n_channel_volumes[r->device],
				this->channel_map[r->device]);
		spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
		spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float,
				this->n_channel_volumes[r->device],
				this->channel_volumes[r->device]);

		spa_pod_builder_prop(b, SPA_PROP_params, 0);
		spa_pod_builder_push_struct(b, &sf);
		spa_pod_builder_string(b, "droid.route");
		spa_pod_builder_string(b, r->pa_name);
		spa_pod_builder_pop(b, &sf);
		spa_pod_builder_pop(b, &pf);
	}

	/* Routes apply in both operating profiles - during a call the choice
	 * between earpiece and speaker is the whole point. */
	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_profiles, 0);
	spa_pod_builder_push_array(b, &f);
	spa_pod_builder_int(b, PROFILE_DEFAULT);
	spa_pod_builder_int(b, PROFILE_VOICECALL);
	spa_pod_builder_int(b, PROFILE_COMMUNICATION);
	spa_pod_builder_pop(b, &f);

	spa_pod_builder_prop(b, SPA_PARAM_ROUTE_devices, 0);
	spa_pod_builder_push_array(b, &f);
	spa_pod_builder_int(b, r->device);
	spa_pod_builder_pop(b, &f);
}

/* -------------------------------------------------------------- nodes */

static void emit_node(struct impl *this, uint32_t device)
{
	struct spa_device_object_info info;
	struct spa_dict_item items[16];
	char routes_str[8], dev_str[8];
	uint32_t n = 0, i, n_routes = 0;
	bool sink = device == DEV_SINK;

	for (i = 0; i < this->n_routes; i++)
		if (this->routes[i].device == device)
			n_routes++;

	snprintf(routes_str, sizeof(routes_str), "%u", n_routes);
	snprintf(dev_str, sizeof(dev_str), "%u", device);

	items[n++] = SPA_DICT_ITEM_INIT("node.name", sink ? "droid-sink" : "droid-source");
	items[n++] = SPA_DICT_ITEM_INIT("node.description",
			sink ? "Phone (Playback)" : "Phone (Capture)");
	items[n++] = SPA_DICT_ITEM_INIT("media.class", sink ? "Audio/Sink" : "Audio/Source");
	items[n++] = SPA_DICT_ITEM_INIT("device.api", DROID_API_NAME);
	items[n++] = SPA_DICT_ITEM_INIT("device.class", "sound");
	items[n++] = SPA_DICT_ITEM_INIT("droid.mix-port", mix_port_of(device));
	items[n++] = SPA_DICT_ITEM_INIT("card.profile.device", dev_str);
	items[n++] = SPA_DICT_ITEM_INIT("device.routes", routes_str);
	items[n++] = SPA_DICT_ITEM_INIT("audio.format", "S16LE");
	items[n++] = SPA_DICT_ITEM_INIT("audio.rate", "48000");
	items[n++] = SPA_DICT_ITEM_INIT("audio.channels", "2");
	items[n++] = SPA_DICT_ITEM_INIT("audio.position", "FL,FR");
	/* Both directions clock their graph themselves; playback should drive
	 * when both are connected. */
	items[n++] = SPA_DICT_ITEM_INIT("node.driver", "true");
	items[n++] = SPA_DICT_ITEM_INIT("priority.driver", sink ? "50000" : "20000");
	/* priority.session MUST be set: without it WirePlumber's device
	 * selection falls back to priority.driver - and at 50000 this node would
	 * beat even an explicit user choice (which is weighted 30000). No other
	 * output device could be selected any more, no Bluetooth headphones,
	 * nothing. 1000 is the usual value for built-in hardware; a connected
	 * headset ranks above it. */
	items[n++] = SPA_DICT_ITEM_INIT("priority.session", "1000");

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = sink ? "api.droid.pcm" : "api.droid.pcm.source";
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT(items, n);

	spa_device_emit_object_info(&this->hooks, device, &info);
}

/* The HAL's VoIP path: its own mix ports, its own processing. Android uses it
 * for voice calls over the network - the HAL applies echo cancellation and
 * noise reduction differently there than for music.
 *
 * Low priority: nothing should end up here by accident. Whoever wants the path
 * picks it explicitly (or a rule sends calls there). */
static void emit_voip_node(struct impl *this, bool sink)
{
	struct spa_device_object_info info;
	struct spa_dict_item items[12];
	uint32_t n = 0;

	items[n++] = SPA_DICT_ITEM_INIT("node.name",
			sink ? "droid-voip-sink" : "droid-voip-source");
	items[n++] = SPA_DICT_ITEM_INIT("node.description",
			sink ? "Phone (VoIP Playback)" : "Phone (VoIP Capture)");
	items[n++] = SPA_DICT_ITEM_INIT("media.class",
			sink ? "Audio/Sink" : "Audio/Source");
	items[n++] = SPA_DICT_ITEM_INIT("device.api", DROID_API_NAME);
	items[n++] = SPA_DICT_ITEM_INIT("droid.mix-port", sink ? "voip_rx" : "voip_tx");
	if (!sink)
		/* Exactly this source makes the HAL turn on its voice processing -
		 * the spelling with a space comes from the conversion table, not
		 * from us. */
		items[n++] = SPA_DICT_ITEM_INIT("droid.audio-source", "voice communication");
	items[n++] = SPA_DICT_ITEM_INIT("audio.format", "S16LE");
	/* 16 kHz, not 48: the HAL enforces exactly that for voip_rx ("Override
	 * voip_rx channel map (mono) and sample rate (16000)"). If we offered
	 * 48 kHz the node would write into it at three times the speed. */
	items[n++] = SPA_DICT_ITEM_INIT("audio.rate", "16000");
	items[n++] = SPA_DICT_ITEM_INIT("audio.channels", sink ? "2" : "1");
	items[n++] = SPA_DICT_ITEM_INIT("audio.position", sink ? "FL,FR" : "MONO");
	items[n++] = SPA_DICT_ITEM_INIT("node.driver", "true");
	items[n++] = SPA_DICT_ITEM_INIT("priority.session", "500");

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = sink ? "api.droid.pcm" : "api.droid.pcm.source";
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT(items, n);

	spa_device_emit_object_info(&this->hooks,
			sink ? OBJ_VOIP_SINK : OBJ_VOIP_SOURCE, &info);
}

static void emit_nodes(struct impl *this, bool present)
{
	uint32_t i;

	if (present == this->nodes_emitted)
		return;

	if (present) {
		emit_node(this, DEV_SINK);
		emit_node(this, DEV_SOURCE);
		emit_voip_node(this, true);
		emit_voip_node(this, false);
	} else {
		for (i = 0; i < N_DEVICES; i++)
			spa_device_emit_object_info(&this->hooks, i, NULL);
		spa_device_emit_object_info(&this->hooks, OBJ_VOIP_SINK, NULL);
		spa_device_emit_object_info(&this->hooks, OBJ_VOIP_SOURCE, NULL);
	}
	this->nodes_emitted = present;
}

static void emit_info(struct impl *this, bool full)
{
	uint64_t old = full ? this->info.change_mask : 0;
	struct spa_dict_item items[8];
	uint32_t n = 0;

	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, DROID_API_NAME);
	items[n++] = SPA_DICT_ITEM_INIT("device.class", "sound");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME, "droid");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION, device_description());
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NICK, "droid");
	items[n++] = SPA_DICT_ITEM_INIT("api.droid.module",
			this->module ? this->module->name : "primary");
	this->info.props = &SPA_DICT_INIT(items, n);

	if (full)
		this->info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS |
					 SPA_DEVICE_CHANGE_MASK_PARAMS;
	if (this->info.change_mask) {
		spa_device_emit_info(&this->hooks, &this->info);
		this->info.change_mask = old;
	}
	this->info.props = NULL;
}

/* -------------------------------------------------------------- Params */

static int impl_enum_params(void *object, int seq, uint32_t id,
		uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct impl *this = object;
	struct spa_pod_builder b = { 0 };
	uint8_t buffer[2048];
	struct spa_result_device_params result;
	uint32_t count = 0;
	int res;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	result.id = id;
	result.next = start;
next:
	result.index = result.next++;
	spa_pod_builder_init(&b, buffer, sizeof(buffer));

	switch (id) {
	case SPA_PARAM_EnumProfile:
		if ((res = build_profile(this, &b, id, result.index, false,
						(struct spa_pod **) &result.param)) <= 0)
			return res;
		break;
	case SPA_PARAM_Profile:
		if (result.index > 0)
			return 0;
		if ((res = build_profile(this, &b, id, this->profile, true,
						(struct spa_pod **) &result.param)) <= 0)
			return res;
		break;
	case SPA_PARAM_EnumRoute:
	{
		struct spa_pod_frame f;
		if (result.index >= this->n_routes)
			return 0;
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamRoute, id);
		build_route_body(this, &b, &this->routes[result.index], result.index, false);
		result.param = spa_pod_builder_pop(&b, &f);
		break;
	}
	case SPA_PARAM_Route:
	{
		struct spa_pod_frame f;
		uint32_t dev = result.index, idx;
		if (dev >= N_DEVICES)
			return 0;
		if ((idx = this->active[dev]) == SPA_ID_INVALID)
			goto next;
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamRoute, id);
		build_route_body(this, &b, &this->routes[idx], idx, true);
		spa_pod_builder_add(&b, SPA_PARAM_ROUTE_save, SPA_POD_Bool(true), 0);
		result.param = spa_pod_builder_pop(&b, &f);
		break;
	}
	default:
		return -ENOENT;
	}

	if (spa_pod_filter(&b, (struct spa_pod **) &result.param, result.param, filter) < 0)
		goto next;

	spa_device_emit_result(&this->hooks, seq, 0, SPA_RESULT_TYPE_DEVICE_PARAMS, &result);

	if (++count != num)
		goto next;
	return 0;
}

static void params_changed(struct impl *this, uint32_t id)
{
	uint32_t i;
	/* A param change is announced by flipping the SERIAL bit in the flags -
	 * that is exactly what it is for ("signal update even when the
	 * read/write flags don't change"). The neighbouring `user` field is
	 * private plugin state and is never looked at by PipeWire. */
	for (i = 0; i < SPA_N_ELEMENTS(this->params); i++)
		if (this->params[i].id == id)
			this->params[i].flags ^= SPA_PARAM_INFO_SERIAL;
	this->info.change_mask |= SPA_DEVICE_CHANGE_MASK_PARAMS;
	emit_info(this, false);
}

static int set_profile(struct impl *this, uint32_t index, bool save)
{
	this->profile_save = save;
	if (index > PROFILE_COMMUNICATION)
		return -EINVAL;
	if (index == this->profile)
		return 0;

	this->profile = index;
	/* The nodes survive the profile switch - during a call the same HAL
	 * stream is used, only in mode AUDIO_MODE_IN_CALL. */
	emit_nodes(this, index != PROFILE_OFF);
	DIAG(this, "profile: %s",
			index == PROFILE_VOICECALL ? VOICECALL_NAME :
			index == PROFILE_COMMUNICATION ? "communication" :
			index == PROFILE_DEFAULT ? "default" : "off");
	params_changed(this, SPA_PARAM_Profile);
	return 0;
}

/* Take over volume and mute from a route the graph has just set.
 *
 * Everything that changes the volume of a card-backed node arrives here -
 * pactl through pipewire-pulse, and WirePlumber when it restores a stored
 * level. We only keep the values and hand them back out in the route; the
 * gain itself is applied in the graph, which is where it already worked
 * before any of this existed. */
static void apply_route_props(struct impl *this, uint32_t device,
		const struct spa_pod *props)
{
	struct spa_pod_object *obj = (struct spa_pod_object *) props;
	struct spa_pod_prop *prop;

	if (device >= N_DEVICES || !spa_pod_is_object_type(props, SPA_TYPE_OBJECT_Props))
		return;

	SPA_POD_OBJECT_FOREACH(obj, prop) {
		switch (prop->key) {
		case SPA_PROP_volume:
			spa_pod_get_float(&prop->value, &this->volume[device]);
			break;
		case SPA_PROP_mute:
			spa_pod_get_bool(&prop->value, &this->mute[device]);
			break;
		case SPA_PROP_channelVolumes:
		{
			uint32_t n, i;
			n = spa_pod_copy_array(&prop->value, SPA_TYPE_Float,
					this->channel_volumes[device], MAX_CHANNELS);
			/* Both nodes are stereo, so the count stays at two even when
			 * fewer values arrive - a stored state from an earlier version
			 * held a single channel, and taking that at face value left the
			 * right channel of the microphone at zero. */
			for (i = n; i < this->n_channel_volumes[device]; i++)
				this->channel_volumes[device][i] =
					n > 0 ? this->channel_volumes[device][n - 1] : 1.0f;
			break;
		}
		case SPA_PROP_channelMap:
		{
			uint32_t map[MAX_CHANNELS], n;
			/* Same reasoning as above: a shorter map would rename the
			 * channels, which is how front-left/front-right once turned into
			 * the nameless aux0/aux1. */
			n = spa_pod_copy_array(&prop->value, SPA_TYPE_Id, map, MAX_CHANNELS);
			if (n == this->n_channel_volumes[device])
				memcpy(this->channel_map[device], map, n * sizeof(uint32_t));
			break;
		}
		default:
			break;
		}
	}
	DIAG(this, "route props for device %u: volume %.3f, %u channels, %s",
			device, this->volume[device], this->n_channel_volumes[device],
			this->mute[device] ? "muted" : "not muted");
}

static int set_route(struct impl *this, uint32_t index, uint32_t device)
{
	struct route *r;
	int res;

	if (index >= this->n_routes || device >= N_DEVICES)
		return -EINVAL;
	r = &this->routes[index];
	if (r->device != device)
		return -EINVAL;

	this->active[device] = index;
	/* r->pa_name, NOT r->port->name: the node looks the route up by the name
	 * pa_droid_output_port_name() gives a device type, and that is the route
	 * name. The port name from the vendor's XML never matches it, so every
	 * route set this way was quietly dropped with -ENOENT - which is why a
	 * route only ever arrived while the node was running, through the props
	 * on the active route. This path exists precisely for the other case. */
	res = droid_node_set_route(mix_port_of(device), r->pa_name);
	if (res < 0 && res != -ENOENT)
		spa_log_warn(this->log, NAME " route \"%s\" not applied: %s",
				r->pa_name, spa_strerror(res));
	else
		DIAG(this, "route: %s -> %s", r->pa_name, r->port->name);

	params_changed(this, SPA_PARAM_Route);
	return 0;
}

static int impl_set_param(void *object, uint32_t id, uint32_t flags,
		const struct spa_pod *param)
{
	struct impl *this = object;

	spa_return_val_if_fail(this != NULL, -EINVAL);

	switch (id) {
	case SPA_PARAM_Profile:
	{
		uint32_t index;
		bool save = false;
		if (param == NULL)
			return -EINVAL;
		if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamProfile, NULL,
					SPA_PARAM_PROFILE_index, SPA_POD_Int(&index),
					SPA_PARAM_PROFILE_save, SPA_POD_OPT_Bool(&save)) < 0)
			return -EINVAL;
		return set_profile(this, index, save);
	}
	case SPA_PARAM_Route:
	{
		uint32_t index, device = 0;
		struct spa_pod *props = NULL;
		if (param == NULL)
			return -EINVAL;
		if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
					SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
					SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int(&device),
					SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0)
			return -EINVAL;
		if (props != NULL)
			apply_route_props(this, device, props);
		return set_route(this, index, device);
	}
	default:
		return -ENOENT;
	}
}

static int impl_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_info(this, true);
	if (this->profile == PROFILE_DEFAULT) {
		this->nodes_emitted = false;
		emit_nodes(this, true);
	}

	spa_hook_list_join(&this->hooks, &save);
	return 0;
}

static int impl_sync(void *object, int seq)
{
	struct impl *this = object;
	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_device_emit_result(&this->hooks, seq, 0, 0, NULL);
	return 0;
}

static const struct spa_device_methods impl_device = {
	SPA_VERSION_DEVICE_METHODS,
	.add_listener = impl_add_listener,
	.sync = impl_sync,
	.enum_params = impl_enum_params,
	.set_param = impl_set_param,
};

static int impl_get_interface(struct spa_handle *handle, const char *type, void **interface)
{
	struct impl *this = (struct impl *) handle;

	spa_return_val_if_fail(handle != NULL, -EINVAL);
	spa_return_val_if_fail(interface != NULL, -EINVAL);

	if (spa_streq(type, SPA_TYPE_INTERFACE_Device))
		*interface = &this->device;
	else
		return -ENOENT;

	return 0;
}

static int impl_clear(struct spa_handle *handle)
{
	struct impl *this = (struct impl *) handle;

	if (this->config) {
		dm_config_free(this->config);
		this->config = NULL;
	}
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
	const char *file = NULL;
	uint32_t i;

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);
	{
		const char *env = getenv("SPA_DROID_DIAG");
		this->diag = env && spa_atob(env);
	}

	spa_hook_list_init(&this->hooks);
	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

	this->info = SPA_DEVICE_INFO_INIT();
	this->params[0] = SPA_PARAM_INFO(SPA_PARAM_EnumProfile, SPA_PARAM_INFO_READ);
	this->params[1] = SPA_PARAM_INFO(SPA_PARAM_Profile, SPA_PARAM_INFO_READWRITE);
	this->params[2] = SPA_PARAM_INFO(SPA_PARAM_EnumRoute, SPA_PARAM_INFO_READ);
	this->params[3] = SPA_PARAM_INFO(SPA_PARAM_Route, SPA_PARAM_INFO_READWRITE);
	this->info.params = this->params;
	this->info.n_params = SPA_N_ELEMENTS(this->params);

	if (info)
		file = spa_dict_lookup(info, "droid.config");
	if (!file)
		file = default_config_file();

	this->config = pa_parse_droid_audio_config(file);
	if (!this->config) {
		spa_log_error(this->log, NAME " could not read %s", file);
		return -EIO;
	}

	this->module = dm_config_find_module(this->config, "primary");
	if (!this->module) {
		spa_log_error(this->log, NAME " no \"primary\" module in %s", file);
		dm_config_free(this->config);
		this->config = NULL;
		return -ENOENT;
	}

	collect_routes(this);
	for (i = 0; i < N_DEVICES; i++) {
		uint32_t c;
		this->active[i] = default_route(this, i);
		/* Full volume until somebody says otherwise - the same starting
		 * point PulseAudio uses for a card it has never seen. */
		this->volume[i] = 1.0f;
		this->mute[i] = false;
		this->n_channel_volumes[i] = 2;
		for (c = 0; c < MAX_CHANNELS; c++) {
			this->channel_volumes[i][c] = 1.0f;
			this->channel_map[i][c] = SPA_AUDIO_CHANNEL_UNKNOWN;
		}
		this->channel_map[i][0] = SPA_AUDIO_CHANNEL_FL;
		this->channel_map[i][1] = SPA_AUDIO_CHANNEL_FR;
	}
	this->profile = PROFILE_DEFAULT;

	spa_log_info(this->log, NAME " HAL configuration loaded: %s (%u routes)",
			file, this->n_routes);
	for (i = 0; i < this->n_routes; i++)
		spa_log_info(this->log, NAME "   route %u: %-24s (%s, device %u, prio %u)",
				i, this->routes[i].pa_name, this->routes[i].port->name,
				this->routes[i].device, this->routes[i].priority);
	return 0;
}

static const struct spa_interface_info impl_interfaces[] = {
	{ SPA_TYPE_INTERFACE_Device, },
};

static int impl_enum_interface_info(const struct spa_handle_factory *factory,
		const struct spa_interface_info **info, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(info != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	if (*index >= SPA_N_ELEMENTS(impl_interfaces))
		return 0;

	*info = &impl_interfaces[(*index)++];
	return 1;
}

static const struct spa_handle_factory droid_device_factory = {
	SPA_VERSION_HANDLE_FACTORY,
	.name = "api.droid.device",
	.info = NULL,
	.get_size = impl_get_size,
	.init = impl_init,
	.enum_interface_info = impl_enum_interface_info,
};

/* from droid-pcm.c */
extern const struct spa_handle_factory droid_pcm_factory;
extern const struct spa_handle_factory droid_pcm_source_factory;

SPA_EXPORT
int spa_handle_factory_enum(const struct spa_handle_factory **factory, uint32_t *index)
{
	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(index != NULL, -EINVAL);

	switch (*index) {
	case 0:
		*factory = &droid_device_factory;
		break;
	case 1:
		*factory = &droid_pcm_factory;
		break;
	case 2:
		*factory = &droid_pcm_source_factory;
		break;
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
