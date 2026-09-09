/* SPA-Device fuer den Android-Audio-HAL.
 * Stufe 2a: meldet die HAL-Topologie (mixPorts/devicePorts aus der
 * audio_policy-XML) in den PipeWire-Graphen. Oeffnet den HAL noch nicht -
 * der ist exklusiv und gehoert zur Laufzeit noch PulseAudio. */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <spa/support/plugin.h>
#include <spa/support/log.h>
#include <spa/utils/names.h>
#include <spa/utils/hook.h>
#include <spa/utils/keys.h>
#include <spa/utils/string.h>
#include <spa/node/node.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

#include <hardware/audio.h>
#include "droid/droid-config.h"
#include "droid/conversion.h"
#include "droid/sllist.h"

#define NAME "droid-device"

struct impl {
	struct spa_handle handle;
	struct spa_device device;

	struct spa_log *log;
	struct spa_hook_list hooks;

	dm_config_device *config;
	dm_config_module *module;
};

static const char *default_config_file(void)
{
	return "/android/vendor/etc/audio_policy_configuration.xml";
}

/* Ein mixPort wird zu einem Knoten im Graphen. */
static void emit_node(struct impl *this, dm_config_port *port, uint32_t id)
{
	struct spa_device_object_info info;
	struct spa_dict_item items[6];
	char flags_str[32], id_str[16];
	uint32_t n = 0;
	const char *media_class;
	char *flag_names;

	/* mixPort-Rolle ist aus Sicht des HAL gedacht: SOURCE liefert Wiedergabe. */
	media_class = port->role == DM_CONFIG_ROLE_SOURCE
		? "Audio/Sink" : "Audio/Source";

	snprintf(flags_str, sizeof(flags_str), "%#x", port->flags);
	snprintf(id_str, sizeof(id_str), "%u", id);
	/* pa_list_string_flags kennt nur AUDIO_OUTPUT_FLAG_*; fuer Eingaenge
	 * waeren das AUDIO_INPUT_FLAG_* und die Namen waeren schlicht falsch. */
	flag_names = port->role == DM_CONFIG_ROLE_SOURCE
		? pa_list_string_flags(port->flags) : NULL;

	items[n++] = SPA_DICT_ITEM_INIT("node.name", port->name);
	items[n++] = SPA_DICT_ITEM_INIT("node.description", port->name);
	items[n++] = SPA_DICT_ITEM_INIT("media.class", media_class);
	items[n++] = SPA_DICT_ITEM_INIT("droid.mix-port", port->name);
	items[n++] = SPA_DICT_ITEM_INIT("droid.flags", flags_str);
	if (flag_names)
		items[n++] = SPA_DICT_ITEM_INIT("droid.flag-names", flag_names);

	info = SPA_DEVICE_OBJECT_INFO_INIT();
	info.type = SPA_TYPE_INTERFACE_Node;
	info.factory_name = "api.droid.pcm";
	info.change_mask = SPA_DEVICE_OBJECT_CHANGE_MASK_PROPS;
	info.props = &SPA_DICT_INIT(items, n);

	spa_device_emit_object_info(&this->hooks, id, &info);

	free(flag_names);
}

static void emit_info(struct impl *this)
{
	struct spa_device_info info;
	struct spa_dict_item items[5];
	uint32_t n = 0;

	info = SPA_DEVICE_INFO_INIT();
	info.change_mask = SPA_DEVICE_CHANGE_MASK_PROPS;

	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_API, "droid");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_MEDIA_CLASS, "Audio/Device");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_NAME,
			this->module ? this->module->name : "droid");
	items[n++] = SPA_DICT_ITEM_INIT(SPA_KEY_DEVICE_DESCRIPTION,
			"Android HAL (droid)");
	info.props = &SPA_DICT_INIT(items, n);

	spa_device_emit_info(&this->hooks, &info);
}

static int impl_add_listener(void *object, struct spa_hook *listener,
		const struct spa_device_events *events, void *data)
{
	struct impl *this = object;
	struct spa_hook_list save;
	void *state = NULL;
	dm_config_port *port;
	uint32_t id = 0;

	spa_return_val_if_fail(this != NULL, -EINVAL);
	spa_return_val_if_fail(events != NULL, -EINVAL);

	spa_hook_list_isolate(&this->hooks, &save, listener, events, data);

	emit_info(this);

	if (this->module) {
		for (port = dm_list_first_data(this->module->mix_ports, &state); port;
		     port = dm_list_next_data(this->module->mix_ports, &state))
			emit_node(this, port, id++);
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

	spa_return_val_if_fail(factory != NULL, -EINVAL);
	spa_return_val_if_fail(handle != NULL, -EINVAL);

	handle->get_interface = impl_get_interface;
	handle->clear = impl_clear;

	this = (struct impl *) handle;
	this->log = spa_support_find(support, n_support, SPA_TYPE_INTERFACE_Log);

	spa_hook_list_init(&this->hooks);
	this->device.iface = SPA_INTERFACE_INIT(
			SPA_TYPE_INTERFACE_Device,
			SPA_VERSION_DEVICE,
			&impl_device, this);

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

	spa_log_info(this->log, NAME " HAL-Konfiguration geladen: %s (%zd mixPorts, %zd devicePorts)",
			file,
			(ssize_t) dm_list_size(this->module->mix_ports),
			(ssize_t) dm_list_size(this->module->device_ports));
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
	default:
		return 0;
	}
	(*index)++;
	return 1;
}
