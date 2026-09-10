/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Opens the Android audio HAL through the ported code.
 *
 * CAUTION: the HAL may be exclusive. While PulseAudio is running this call
 * can fail - or, worse, disturb audio that is currently playing. Hence two
 * stages, selected by argument:
 *
 *   --open-module   audio_hw_device_open only (no stream, no routing)
 *   --open-stream   additionally open an output stream
 *
 * Without an argument only the configuration is read and nothing opened.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/audio.h>

/* droid-util.h assumes these types are already known - in PulseAudio they
 * come in via pulsecore/core.h. */
#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>
#include "pulsecore/core.h"
#include "pulsecore/refcnt.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"

#include "droid/droid-util.h"
#include "droid/droid-config.h"
#include "droid/sllist.h"

int main(int argc, char **argv)
{
	dm_config_device *config;
	dm_config_module *module;
	pa_droid_hw_module *hw;
	bool open_module = false, open_stream = false;
	int i;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--open-module")) open_module = true;
		else if (!strcmp(argv[i], "--open-stream")) { open_module = open_stream = true; }
	}

	printf("1) reading configuration ...\n");
	config = pa_parse_droid_audio_config("/android/vendor/etc/audio_policy_configuration.xml");
	if (!config) { fprintf(stderr, "   failed\n"); return 1; }
	module = dm_config_find_module(config, "primary");
	printf("   OK: module \"%s\", %zd mix ports\n", module->name,
			(ssize_t) dm_list_size(module->mix_ports));

	if (!open_module) {
		printf("\nHAL is NOT opened (no --open-module given).\n");
		dm_config_free(config);
		return 0;
	}

	printf("\n2) opening the HAL (audio_hw_device_open) ...\n");
	hw = pa_droid_hw_module_get(pa_compat_core(), config, "primary");
	if (!hw) {
		printf("   FAILED - the HAL is busy or unreachable.\n");
		printf("   That is the expected answer while PulseAudio is running:\n");
		printf("   it would mean the HAL is exclusive.\n");
		dm_config_free(config);
		return 2;
	}
	printf("   OK - HAL module opened.\n");
	printf("\n   NOTE, two separate levels:\n");
	printf("   - The HAL module is NOT exclusive: every process gets its own\n");
	printf("     audio_hw_device instance.\n");
	printf("   - The PCM device (/dev/snd/pcmC0D0p) very much is. If the line\n");
	printf("     \"pcm_hw_open: cannot open device\" appeared above, PulseAudio\n");
	printf("     already holds it - then no sound comes out despite an open HAL.\n");
	printf("   Only --open-stream tells you whether playback is really possible.\n");

	if (open_stream) {
		pa_droid_stream *s;
		/* Ports have to come from hw->enabled_module, not from our own
		 * copy - open_output_stream compares by pointer identity. */
		dm_config_port *mix = dm_config_find_mix_port(hw->enabled_module, "primary output");
		dm_config_port *dev = dm_config_default_output_device(hw->enabled_module);
		pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2 };
		pa_channel_map map;
		pa_channel_map_init_stereo(&map);

		printf("\n3) opening output stream (%s -> %s) ...\n",
				mix ? mix->name : "?", dev ? dev->name : "?");
		s = pa_droid_open_output_stream(hw, &spec, &map, mix, dev);
		if (!s) {
			printf("   failed.\n");
		} else {
			printf("   OK - stream open.\n");
			pa_droid_stream_unref(s);
			printf("   stream closed again.\n");
		}
	}

	pa_droid_hw_module_unref(hw);
	dm_config_free(config);
	printf("\ndone, everything released again.\n");
	return 0;
}
