/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Plays a tone through the Android HAL onto the Bluetooth SCO path.
 *
 * Why this exists: this device's audio_policy_configuration.xml has its whole
 * Bluetooth SCO section commented out (lines 167-217), so the parsed
 * configuration has no such device port and every route to it falls back to
 * the speaker - silently. The HAL itself does not read that file; it takes the
 * device as a bitmask on open_output_stream. So we hand it the bitmask
 * directly and see whether sound comes out of the headset.
 *
 * The PCM device is exclusive, the HAL module is not: while PipeWire holds the
 * primary output (pcmC0D0p) this tool can hold BTCVSD (pcmC0D55p) at the same
 * time. The air link has to be up - something must keep a stream on the
 * headset's hands-free profile while this runs.
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
	struct audio_stream_out *out = NULL;
	struct audio_config cfg;
	audio_devices_t device = AUDIO_DEVICE_OUT_BLUETOOTH_SCO;
	unsigned rate = 8000, seconds = 10, freq = 440, channels = 1;
	const char *wbs = NULL;
	int16_t *tone;
	size_t frames, i, chunk;
	int ret, wrote = 0;

	for (i = 1; i < (size_t) argc; i++) {
		if (!strcmp(argv[i], "--headset")) device = AUDIO_DEVICE_OUT_BLUETOOTH_SCO_HEADSET;
		else if (!strcmp(argv[i], "--wideband")) rate = 16000;
		/* The codec the HAL encodes with. Without this it always encodes
		 * narrow-band CVSD - and when PipeWire has negotiated mSBC on the
		 * air (profile headset-head-unit, 16 kHz) the two do not match and
		 * nothing intelligible reaches the ear. Measured: same tone, same
		 * everything, audible on a CVSD link and silent on an mSBC one. */
		else if (!strcmp(argv[i], "--wbs")) wbs = "on";
		else if (!strcmp(argv[i], "--no-wbs")) wbs = "off";
		else if (!strcmp(argv[i], "--rate") && i + 1 < (size_t) argc) rate = atoi(argv[++i]);
		else if (!strcmp(argv[i], "--stereo")) channels = 2;
		else if (!strcmp(argv[i], "--seconds") && i + 1 < (size_t) argc) seconds = atoi(argv[++i]);
		else { fprintf(stderr, "usage: %s [--headset] [--wideband] [--wbs|--no-wbs] "
				"[--rate N] [--stereo] [--seconds N]\n", argv[0]); return 1; }
	}

	printf("1) reading the HAL configuration ...\n");
	config = pa_parse_droid_audio_config("/android/vendor/etc/audio_policy_configuration.xml");
	if (!config) { fprintf(stderr, "   failed\n"); return 1; }

	printf("2) opening the HAL module ...\n");
	hw = pa_droid_hw_module_get(pa_compat_core(), config, "primary");
	if (!hw) { fprintf(stderr, "   failed - HAL busy or unreachable\n"); return 2; }

	printf("3) set_parameters(BT_SCO=on)\n");
	if (pa_droid_set_parameters(hw, "BT_SCO=on") < 0)
		fprintf(stderr, "   the HAL did not take it\n");

	if (wbs) {
		char param[32];
		snprintf(param, sizeof(param), "bt_wbs=%s", wbs);
		printf("   set_parameters(%s)\n", param);
		if (pa_droid_set_parameters(hw, param) < 0)
			fprintf(stderr, "   the HAL did not take %s\n", param);
	}

	memset(&cfg, 0, sizeof(cfg));
	cfg.sample_rate = rate;
	cfg.channel_mask = channels == 2 ? AUDIO_CHANNEL_OUT_STEREO : AUDIO_CHANNEL_OUT_MONO;
	cfg.format = AUDIO_FORMAT_PCM_16_BIT;

	printf("4) open_output_stream(device=0x%x, %u Hz, %u channels) ...\n", device, rate, channels);
	pa_droid_hw_module_lock(hw);
	ret = hw->device->open_output_stream(hw->device, 0, device,
			AUDIO_OUTPUT_FLAG_PRIMARY, &cfg, &out, NULL);
	pa_droid_hw_module_unlock(hw);
	if (ret < 0 || !out) {
		fprintf(stderr, "   FAILED: %d - the HAL refuses this device\n", ret);
		return 3;
	}
	printf("   OK - the HAL took it: %u Hz, mask 0x%x, format 0x%x\n",
	       cfg.sample_rate, cfg.channel_mask, cfg.format);
	if (cfg.sample_rate && cfg.sample_rate != rate) {
		printf("   (the HAL asked for %u Hz instead)\n", cfg.sample_rate);
		rate = cfg.sample_rate;
	}

	/* A rising and falling tone, so it cannot be mistaken for anything else. */
	frames = (size_t) rate * seconds * channels;
	tone = calloc(frames, sizeof(int16_t));
	for (i = 0; i < frames; i++) {
		double t = (double) (i / channels) / rate;
		double f = freq + 200.0 * sin(2.0 * M_PI * 0.5 * t);
		tone[i] = (int16_t) (10000.0 * sin(2.0 * M_PI * f * t));
	}

	chunk = out->common.get_buffer_size(&out->common);
	if (chunk == 0 || chunk > frames * sizeof(int16_t))
		chunk = 320;
	printf("5) writing %u s in chunks of %zu B ...\n", seconds, chunk);

	for (i = 0; i * chunk < frames * sizeof(int16_t); i++) {
		size_t left = frames * sizeof(int16_t) - i * chunk;
		ssize_t n = out->write(out, ((uint8_t *) tone) + i * chunk,
				left < chunk ? left : chunk);
		if (n < 0) {
			fprintf(stderr, "   write failed after %d chunks: %zd\n", wrote, n);
			break;
		}
		wrote++;
	}
	printf("   %d chunks written\n", wrote);

	pa_droid_hw_module_lock(hw);
	hw->device->close_output_stream(hw->device, out);
	pa_droid_hw_module_unlock(hw);
	free(tone);
	printf("done.\n");
	return 0;
}
