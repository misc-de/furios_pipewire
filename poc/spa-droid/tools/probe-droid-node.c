/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Reproduces what the PipeWire adapter does with the node: enumerate params,
 * set a format, enumerate again. Without PipeWire, without the HAL. */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <spa/support/plugin.h>
#include <spa/support/log-impl.h>
#include <spa/utils/hook.h>
#include <spa/utils/names.h>
#include <spa/node/node.h>
#include <spa/node/utils.h>
#include <spa/utils/result.h>
#include <spa/param/param.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>

SPA_LOG_IMPL(default_log);

static void on_result(void *data, int seq, int res, uint32_t type, const void *result)
{
	int *n = data;
	(*n)++;
}

static const struct spa_node_events node_events = {
	SPA_VERSION_NODE_EVENTS,
	.result = on_result,
};

static const char *pname(uint32_t id)
{
	switch (id) {
	case SPA_PARAM_EnumFormat: return "EnumFormat";
	case SPA_PARAM_Format:     return "Format";
	case SPA_PARAM_Buffers:    return "Buffers";
	case SPA_PARAM_IO:         return "IO";
	default:                   return "?";
	}
}

int main(int argc, char **argv)
{
	const char *path = argc > 1 ? argv[1] : "./libspa-droid.so";
	void *dl = dlopen(path, RTLD_NOW);
	spa_handle_factory_enum_func_t ef;
	const struct spa_handle_factory *f = NULL;
	uint32_t index = 0;
	struct spa_handle *h;
	struct spa_node *node;
	struct spa_support support[1];
	struct spa_hook listener;
	int res, count = 0;
	uint32_t params[] = { SPA_PARAM_EnumFormat, SPA_PARAM_Format,
			      SPA_PARAM_Buffers, SPA_PARAM_IO };
	unsigned i;

	if (!dl) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
	ef = dlsym(dl, SPA_HANDLE_FACTORY_ENUM_FUNC_NAME);

	while (ef(&f, &index) > 0)
		if (!strcmp(f->name, "api.droid.pcm")) break;
	printf("Factory: %s\n", f->name);

	support[0] = SPA_SUPPORT_INIT(SPA_TYPE_INTERFACE_Log, &default_log.log);
	h = calloc(1, f->get_size(f, NULL));
	if ((res = f->init(f, h, NULL, support, 1)) < 0) {
		fprintf(stderr, "init: %s\n", strerror(-res)); return 1;
	}
	printf("init OK\n");

	spa_handle_get_interface(h, SPA_TYPE_INTERFACE_Node, (void **) &node);

	printf("add_listener ...\n"); fflush(stdout);
	spa_zero(listener);
	spa_node_add_listener(node, &listener, &node_events, &count);
	printf("   OK\n");

	for (i = 0; i < SPA_N_ELEMENTS(params); i++) {
		printf("port_enum_params(%s) ...\n", pname(params[i])); fflush(stdout);
		res = spa_node_port_enum_params(node, 0, SPA_DIRECTION_INPUT, 0,
				params[i], 0, 4, NULL);
		printf("   -> %d (%s)\n", res, res < 0 ? spa_strerror(res) : "ok");
	}

	printf("port_set_param(Format) ...\n"); fflush(stdout);
	{
		uint8_t buf[512];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		struct spa_audio_info_raw raw = {
			.format = SPA_AUDIO_FORMAT_S16_LE,
			.rate = 48000, .channels = 2,
			.position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR },
		};
		struct spa_pod *fmt = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &raw);
		res = spa_node_port_set_param(node, SPA_DIRECTION_INPUT, 0,
				SPA_PARAM_Format, 0, fmt);
		printf("   -> %d (%s)\n", res, res < 0 ? spa_strerror(res) : "ok");
	}

	printf("port_enum_params(Buffers) nach Format ...\n"); fflush(stdout);
	res = spa_node_port_enum_params(node, 0, SPA_DIRECTION_INPUT, 0,
			SPA_PARAM_Buffers, 0, 4, NULL);
	printf("   -> %d (%s)\n", res, res < 0 ? spa_strerror(res) : "ok");

	printf("\nAlles ueberstanden, %d Ergebnisse. HAL wurde nie geoeffnet.\n", count);
	spa_handle_clear(h);
	free(h);
	return 0;
}
