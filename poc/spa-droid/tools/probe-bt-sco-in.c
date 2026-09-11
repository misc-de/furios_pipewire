/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Records from the Bluetooth headset microphone through the Android HAL.
 *
 * The counterpart to probe-bt-sco-out.c: the device port is missing from this
 * phone's audio policy (commented out), so the device type is handed to the
 * HAL directly. Measures what comes back, so "it recorded nothing" and "it
 * recorded silence" can be told apart.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <hardware/audio.h>

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulse/proplist.h>
#include "pulsecore/core.h"
#include "pulsecore/refcnt.h"
#include "pulsecore/hashmap.h"
#include "pulsecore/idxset.h"

#include "droid/droid-util.h"
#include "droid/droid-config.h"

int main(int argc, char **argv)
{
	dm_config_device *config;
	pa_droid_hw_module *hw;
	struct audio_stream_in *in = NULL;
	struct audio_config cfg;
	audio_devices_t device = AUDIO_DEVICE_IN_BLUETOOTH_SCO_HEADSET;
	unsigned rate = 8000, seconds = 8;
	const char *out_path = NULL;
	size_t chunk, total = 0;
	int16_t *buf;
	int ret, i;
	double square_sum = 0.0;
	int16_t lo = 32767, hi = -32768;
	unsigned long samples = 0;
	FILE *out = NULL;
	time_t deadline;

	for (i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--wideband")) rate = 16000;
		else if (!strcmp(argv[i], "--builtin")) device = AUDIO_DEVICE_IN_BUILTIN_MIC;
		else if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--out") && i + 1 < argc) out_path = argv[++i];
		else { fprintf(stderr, "usage: %s [--builtin] [--wideband] [--seconds N] [--out FILE]\n", argv[0]); return 1; }
	}

	config = pa_parse_droid_audio_config("/android/vendor/etc/audio_policy_configuration.xml");
	if (!config) { fprintf(stderr, "reading the configuration failed\n"); return 1; }
	hw = pa_droid_hw_module_get(pa_compat_core(), config, "primary");
	if (!hw) { fprintf(stderr, "the HAL is busy or unreachable\n"); return 2; }

	pa_droid_set_parameters(hw, "BT_SCO=on");

	memset(&cfg, 0, sizeof(cfg));
	cfg.sample_rate = rate;
	cfg.channel_mask = AUDIO_CHANNEL_IN_MONO;
	cfg.format = AUDIO_FORMAT_PCM_16_BIT;

	printf("open_input_stream(device=0x%x, %u Hz, mono) ...\n", device, rate);
	pa_droid_hw_module_lock(hw);
	ret = hw->device->open_input_stream(hw->device, 0, device, &cfg, &in,
			AUDIO_INPUT_FLAG_NONE, NULL, AUDIO_SOURCE_VOICE_COMMUNICATION);
	pa_droid_hw_module_unlock(hw);
	if (ret < 0 || !in) {
		fprintf(stderr, "  FAILED: %d - the HAL refuses this device\n", ret);
		return 3;
	}
	printf("  OK: %u Hz, mask 0x%x\n", cfg.sample_rate, cfg.channel_mask);
	if (cfg.sample_rate) rate = cfg.sample_rate;

	chunk = in->common.get_buffer_size(&in->common);
	if (chunk == 0) chunk = 320;
	buf = malloc(chunk);
	if (out_path && !(out = fopen(out_path, "wb"))) { perror(out_path); return 1; }

	printf("recording %u s ...\n", seconds);
	deadline = time(NULL) + seconds;
	while (time(NULL) < deadline) {
		ssize_t n = in->read(in, buf, chunk);
		size_t k;
		if (n <= 0) {
			fprintf(stderr, "  read returned %zd - stopping\n", n);
			break;
		}
		total += n;
		if (out) fwrite(buf, 1, n, out);
		for (k = 0; k + 1 < (size_t) n; k += 2) {
			int16_t s = buf[k / 2];
			if (s < lo) lo = s;
			if (s > hi) hi = s;
			square_sum += (double) s * s;
			samples++;
		}
	}

	printf("\n%zu bytes read\n", total);
	if (samples)
		printf("min %d, max %d, RMS %.1f\n", lo, hi, sqrt(square_sum / samples));
	/* Not "is it exactly zero": a live but silent link decodes to a few
	 * counts of noise, which is not a microphone that hears anything. */
	if (samples && sqrt(square_sum / samples) < 20.0)
		printf("VERDICT: the path is open, but nothing is speaking into it.\n");
	else if (samples)
		printf("VERDICT: real audio - something is being picked up.\n");
	else
		printf("VERDICT: nothing came back at all.\n");

	if (out) fclose(out);
	pa_droid_hw_module_lock(hw);
	hw->device->close_input_stream(hw->device, in);
	pa_droid_hw_module_unlock(hw);
	free(buf);
	return 0;
}
