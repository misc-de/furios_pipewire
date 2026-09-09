/* Laedt das SPA-Plugin wie PipeWire es tut, haengt sich als Listener an und
 * zeigt, was das Device in den Graphen meldet. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/support/plugin.h>
#include <spa/support/log-impl.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/node/node.h>
#include <spa/monitor/device.h>
#include <spa/monitor/utils.h>

SPA_LOG_IMPL(default_log);

static void on_info(void *data, const struct spa_device_info *info)
{
	uint32_t i;
	printf("Device-Info:\n");
	if (info->props)
		for (i = 0; i < info->props->n_items; i++)
			printf("   %-24s = %s\n", info->props->items[i].key,
					info->props->items[i].value);
}

static void on_object_info(void *data, uint32_t id,
		const struct spa_device_object_info *info)
{
	uint32_t i;
	int *count = data;
	if (!info) { printf("Objekt %u entfernt\n", id); return; }
	(*count)++;
	printf("\nObjekt %u  type=%s  factory=%s\n", id,
			info->type ? info->type : "(keiner)",
			info->factory_name ? info->factory_name : "(keine)");
	if (info->props)
		for (i = 0; i < info->props->n_items; i++)
			printf("   %-24s = %s\n", info->props->items[i].key,
					info->props->items[i].value);
}

static const struct spa_device_events device_events = {
	SPA_VERSION_DEVICE_EVENTS,
	.info = on_info,
	.object_info = on_object_info,
};

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "./libspa-droid.so";
	void *dl;
	spa_handle_factory_enum_func_t enum_func;
	const struct spa_handle_factory *factory = NULL;
	uint32_t index = 0;
	struct spa_handle *handle;
	struct spa_device *device;
	struct spa_hook listener;
	struct spa_support support[1];
	int res, count = 0;

	if (!(dl = dlopen(path, RTLD_NOW))) {
		fprintf(stderr, "dlopen: %s\n", dlerror());
		return 1;
	}
	if (!(enum_func = dlsym(dl, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME))) {
		fprintf(stderr, "kein spa_handle_factory_enum\n");
		return 1;
	}
	printf("Factories im Plugin:\n");
	{
		uint32_t k = 0;
		const struct spa_handle_factory *f;
		while (enum_func(&f, &k) > 0)
			printf("   %s\n", f->name);
	}
	printf("\n");

	support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, &default_log.log);

	/* api.droid.pcm nur initialisieren - ohne Start-Kommando wird der
	 * HAL nicht angefasst. */
	index = 0;
	while (enum_func(&factory, &index) > 0) {
		if (strcmp(factory->name, "api.droid.pcm") != 0)
			continue;
		struct spa_handle *h = calloc(1, factory->get_size(factory, NULL));
		int r = factory->init(factory, h, NULL, support, 1);
		printf("api.droid.pcm init: %s\n", r == 0 ? "OK" : strerror(-r));
		if (r == 0) {
			void *iface = NULL;
			r = spa_handle_get_interface(h, SPA_TYPE_INTERFACE_Node, &iface);
			printf("   Node-Interface: %s\n", r == 0 && iface ? "vorhanden" : "FEHLT");
			spa_handle_clear(h);
		}
		free(h);
		break;
	}
	printf("\n");

	index = 0;
	if (enum_func(&factory, &index) <= 0) {
		fprintf(stderr, "keine Factory\n");
		return 1;
	}
	printf("=== %s ===\n\n", factory->name);

	handle = calloc(1, factory->get_size(factory, NULL));
	if ((res = factory->init(factory, handle, NULL, support, 1)) < 0) {
		fprintf(stderr, "init fehlgeschlagen: %d\n", res);
		return 1;
	}
	if ((res = spa_handle_get_interface(handle, SPA_TYPE_INTERFACE_Device,
					(void **) &device)) < 0) {
		fprintf(stderr, "kein Device-Interface: %d\n", res);
		return 1;
	}

	spa_zero(listener);
	spa_device_add_listener(device, &listener, &device_events, &count);

	printf("\n=> %d Knoten gemeldet\n", count);

	spa_hook_remove(&listener);
	spa_handle_clear(handle);
	free(handle);
	dlclose(dl);
	return 0;
}
