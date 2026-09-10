/* SPA-Device fuer den Android-Audio-HAL.
 *
 * Das Device ist die Karte: es meldet Profile und Routen und erzeugt die
 * beiden Knoten (Wiedergabe/Aufnahme). Die Routennamen sind bewusst genau
 * die, die PulseAudios droid-card verwendet - "output-earpiece",
 * "output-speaker", "input-builtin_mic" - denn callaudiod sucht nach genau
 * diesen Namen, um im Anruf zwischen Ohrmuschel und Lautsprecher zu schalten.
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

/* Genau die Kennung, die PulseAudios droid-Modul setzt (PROP_DROID_API_STRING).
 * callaudiod erkennt eine Android-HAL-Karte allein daran und schaltet nur dann
 * zwischen "default" und "voicecall" um - mit "droid" faellt es auf den
 * UCM-Weg zurueck und tut nichts. */
#define DROID_API_NAME  "droid-hal"

#define MAX_ROUTES  16
#define DEV_SINK    0
#define DEV_SOURCE  1
#define N_DEVICES   2

#define PROFILE_OFF           0
#define PROFILE_DEFAULT       1
#define PROFILE_VOICECALL     2
#define PROFILE_COMMUNICATION 3

/* Der Name ist nicht frei waehlbar: callaudiod sucht im Kartenprofil nach
 * genau diesem Namen ("card has voice profile, using it"). */
#define VOICECALL_NAME  "voicecall"

/* aus droid-pcm.c */
int droid_node_set_route(const char *mix_port, const char *device_port);

struct route {
	dm_config_port *port;      /* devicePort aus der HAL-Konfiguration */
	const char *pa_name;       /* "output-speaker" usw. */
	char description[64];
	enum spa_direction dir;
	uint32_t device;           /* DEV_SINK oder DEV_SOURCE */
	uint32_t priority;
	uint32_t available;        /* SPA_PARAM_AVAILABILITY_* */
};

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	bool diag;                 /* ausfuehrliche Diagnose (SPA_DROID_DIAG=1) */

	struct spa_log *log;
	struct spa_hook_list hooks;

	struct spa_device_info info;
	struct spa_param_info params[4];

	dm_config_device *config;
	dm_config_module *module;

	struct route routes[MAX_ROUTES];
	uint32_t n_routes;
	uint32_t active[N_DEVICES];   /* Index in routes[], SPA_ID_INVALID = keiner */

	uint32_t profile;
	bool profile_save;         /* war es eine bewusste Auswahl? */
	bool nodes_emitted;
};

/* Wie im Knoten: standardmaessig auf info (unter dem Standard-Loglevel
 * unsichtbar), mit SPA_DROID_DIAG=1 auf warn. */
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

/* ------------------------------------------------------------- Routen */

/* Wie wichtig ist ein Port? Lautsprecher und Ohrmuschel sind die Faelle, um
 * die es im Anruf geht; Kabelzubehoer schlaegt sie, wenn es steckt. */
static uint32_t route_priority(audio_devices_t type)
{
	/* Dieselben Werte, die PulseAudios droid-card vergibt. */
	switch (type) {
	case AUDIO_DEVICE_OUT_SPEAKER:
		return 300;
	case AUDIO_DEVICE_OUT_EARPIECE:
	case AUDIO_DEVICE_IN_BUILTIN_MIC:
	case AUDIO_DEVICE_IN_BACK_MIC:
	case AUDIO_DEVICE_IN_VOICE_CALL:
		return 200;
	case AUDIO_DEVICE_OUT_WIRED_HEADSET:
	case AUDIO_DEVICE_OUT_WIRED_HEADPHONE:
	case AUDIO_DEVICE_IN_WIRED_HEADSET:
		return 100;
	default:
		return 50;
	}
}

/* Bluetooth steht in der audio_policy-XML dieses Geraets vollstaendig im
 * Kommentar - ein Block von rund 3900 Zeichen nimmt zehn Geraeteports mit,
 * darunter alle BT-SCO- und A2DP-Eintraege. Der HAL braucht die XML aber
 * nicht: beim Routen bekommt er nur den Geraetetyp, und
 * pa_droid_stream_set_route schickt fuer BT-SCO-Typen zusaetzlich BT_SCO=on
 * an den HAL - genau der Android-Mechanismus, der den Sprachpfad auf die
 * Bluetooth-PCM-Leitung legt. Also bauen wir die Ports selbst.
 *
 * Der Controller dieses Geraets fuehrt SCO nicht ueber HCI (hciconfig zeigt
 * sco:0 in beide Richtungen), sondern in Hardware zwischen BT-Chip und
 * Audio-DSP. Damit ist dieser Weg der einzig moegliche fuer Telefonate ueber
 * ein Headset. */
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
		/* Ob wirklich ein Headset verbunden ist, weiss die Karte nicht - das
		 * entscheidet der Bluetooth-Stack. Also "unbekannt": waehlbar, aber
		 * nichts waehlt sie von selbst. */
		r->available = SPA_PARAM_AVAILABILITY_unknown;
		snprintf(r->description, sizeof(r->description), "Bluetooth (%s)",
				defs[i].role == DM_CONFIG_ROLE_SINK ? "Freisprechen" : "Mikrofon");
	}
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

		/* Ohne PulseAudio-Namen waere die Route fuer callaudiod & Co.
		 * wertlos - solche Ports lassen wir weg. */
		if (output) {
			if (!pa_droid_output_port_name(port->type, &pa_name))
				continue;
		} else {
			if (!pa_droid_input_port_name(port->type, &pa_name))
				continue;
		}

		r = &this->routes[this->n_routes++];
		r->port = port;
		r->pa_name = pa_name;
		r->dir = output ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;
		r->device = output ? DEV_SINK : DEV_SOURCE;
		r->priority = route_priority(port->type);
		/* Kabelzubehoer gilt als nicht angeschlossen. Das ist keine Willkuer,
		 * sondern was PulseAudio auf diesem Geraet ebenfalls meldet - eine
		 * Klinkenerkennung gibt es hier nicht (in /sys/class/extcon steht nur
		 * USB). Und es ist wichtig: callaudiod nimmt bei Droid-Karten sofort
		 * das Headset, sobald es nicht als "nicht verfuegbar" markiert ist -
		 * im Anruf landete der Ton dann statt auf der Ohrmuschel im
		 * Nirgendwo. */
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
		snprintf(r->description, sizeof(r->description), "%s", port->name);
	}

	add_synthetic_bt_routes(this);
}

static uint32_t default_route(struct impl *this, uint32_t device)
{
	uint32_t i, best = SPA_ID_INVALID, best_prio = 0;

	for (i = 0; i < this->n_routes; i++) {
		/* Kabelzubehoer nicht blind vorwaehlen - ob es steckt, wissen wir
		 * (noch) nicht. */
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
		name = "off"; desc = "Aus"; prio = 0;
		break;
	case PROFILE_DEFAULT:
		name = "default"; desc = "Wiedergabe und Aufnahme"; prio = 100;
		break;
	case PROFILE_VOICECALL:
		/* Niedrigere Prioritaet als default - dieses Profil waehlt
		 * callaudiod im Anruf, nicht die Routenpolitik von selbst. */
		name = VOICECALL_NAME; desc = "Anruf"; prio = 50;
		break;
	case PROFILE_COMMUNICATION:
		/* AUDIO_MODE_IN_COMMUNICATION: dafuer schaltet der HAL seine
		 * Echounterdrueckung und Rauschminderung ein. Gedacht fuer VoIP -
		 * heisst wie bei PulseAudios droid-card, damit vorhandene Werkzeuge
		 * es finden. Waehlt niemand von selbst. */
		name = "communication"; desc = "VoIP-Gespraech"; prio = 40;
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

	/* Ohne save-Kennzeichnung haelt WirePlumber die Auswahl fuer eine
	 * beilaeufige Aenderung und legt sie nicht in den Profilzustand.
	 *
	 * Der Anrufmodus ist die Ausnahme: er wird NIE gespeichert. Sonst merkt
	 * sich WirePlumber ein Profil, das beim naechsten Start wieder gesetzt
	 * wuerde - das Telefon startete im Anrufmodus, ohne dass jemand
	 * telefoniert. */
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

		/* Die Route traegt ihren Namen als Prop mit. PipeWire reicht die
		 * Props einer aktiven Route an den Knoten des Geraets weiter - und
		 * nur der Knoten haelt den HAL-Stream, den es umzurouten gilt.
		 * Device und Knoten laufen in verschiedenen Prozessen. */
		spa_pod_builder_prop(b, SPA_PARAM_ROUTE_props, 0);
		spa_pod_builder_push_object(b, &pf, SPA_TYPE_OBJECT_Props, SPA_PARAM_Route);
		spa_pod_builder_prop(b, SPA_PROP_params, 0);
		spa_pod_builder_push_struct(b, &sf);
		spa_pod_builder_string(b, "droid.route");
		spa_pod_builder_string(b, r->pa_name);
		spa_pod_builder_pop(b, &sf);
		spa_pod_builder_pop(b, &pf);
	}

	/* Routen gelten in beiden Betriebsprofilen - im Anruf ist die Wahl
	 * zwischen Ohrmuschel und Lautsprecher gerade der Kern der Sache. */
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

/* ------------------------------------------------------------- Knoten */

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
			sink ? "Android HAL (Wiedergabe)" : "Android HAL (Aufnahme)");
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
	/* Beide Richtungen takten ihren Graphen selbst; die Wiedergabe soll
	 * fuehren, wenn beide verbunden sind. */
	items[n++] = SPA_DICT_ITEM_INIT("node.driver", "true");
	items[n++] = SPA_DICT_ITEM_INIT("priority.driver", sink ? "50000" : "20000");
	/* priority.session MUSS gesetzt sein: fehlt sie, faellt WirePlumbers
	 * Geraetewahl auf priority.driver zurueck - und mit 50000 schluege dieser
	 * Knoten sogar eine ausdrueckliche Benutzerwahl (die mit 30000 gewichtet
	 * wird). Es liesse sich dann kein anderes Ausgabegeraet mehr auswaehlen,
	 * kein Bluetooth-Kopfhoerer, nichts. 1000 ist der uebliche Wert fuer
	 * eingebaute Hardware; ein verbundener Kopfhoerer liegt darueber. */
	items[n++] = SPA_DICT_ITEM_INIT("priority.session", "1000");

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = sink ? "api.droid.pcm" : "api.droid.pcm.source";
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT(items, n);

	spa_device_emit_object_info(&this->hooks, device, &info);
}

static void emit_nodes(struct impl *this, bool present)
{
	uint32_t i;

	if (present == this->nodes_emitted)
		return;

	if (present) {
		emit_node(this, DEV_SINK);
		emit_node(this, DEV_SOURCE);
	} else {
		for (i = 0; i < N_DEVICES; i++)
			spa_device_emit_object_info(&this->hooks, i, NULL);
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
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION, "Android HAL");
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
	/* Eine Param-Aenderung meldet man, indem das SERIAL-Bit in den Flags
	 * kippt - genau dafuer ist es da ("signal update even when the
	 * read/write flags don't change"). Das Feld `user` daneben ist privater
	 * Zustand des Plugins und wird von PipeWire nie angesehen. */
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
	/* Die Knoten bleiben ueber den Profilwechsel hinweg bestehen - im Anruf
	 * wird derselbe HAL-Stream benutzt, nur im Modus AUDIO_MODE_IN_CALL. */
	emit_nodes(this, index != PROFILE_OFF);
	DIAG(this, "Profil: %s",
			index == PROFILE_VOICECALL ? VOICECALL_NAME :
			index == PROFILE_COMMUNICATION ? "communication" :
			index == PROFILE_DEFAULT ? "default" : "off");
	params_changed(this, SPA_PARAM_Profile);
	return 0;
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
	res = droid_node_set_route(mix_port_of(device), r->port->name);
	if (res < 0 && res != -ENOENT)
		spa_log_warn(this->log, NAME " Route \"%s\" nicht angewandt: %s",
				r->pa_name, spa_strerror(res));
	else
		DIAG(this, "Route: %s -> %s", r->pa_name, r->port->name);

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
		if (param == NULL)
			return -EINVAL;
		if (spa_pod_parse_object(param, SPA_TYPE_OBJECT_ParamRoute, NULL,
					SPA_PARAM_ROUTE_index, SPA_POD_Int(&index),
					SPA_PARAM_ROUTE_device, SPA_POD_OPT_Int(&device)) < 0)
			return -EINVAL;
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
		spa_log_error(this->log, NAME " konnte %s nicht lesen", file);
		return -EIO;
	}

	this->module = dm_config_find_module(this->config, "primary");
	if (!this->module) {
		spa_log_error(this->log, NAME " kein Modul \"primary\" in %s", file);
		dm_config_free(this->config);
		this->config = NULL;
		return -ENOENT;
	}

	collect_routes(this);
	for (i = 0; i < N_DEVICES; i++)
		this->active[i] = default_route(this, i);
	this->profile = PROFILE_DEFAULT;

	spa_log_info(this->log, NAME " HAL-Konfiguration geladen: %s (%u Routen)",
			file, this->n_routes);
	for (i = 0; i < this->n_routes; i++)
		spa_log_info(this->log, NAME "   Route %u: %-24s (%s, Geraet %u, Prio %u)",
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

/* aus droid-pcm.c */
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
