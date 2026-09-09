/* Oeffnet den Android-Audio-HAL ueber den portierten Code.
 *
 * ACHTUNG: Der HAL ist moeglicherweise exklusiv. Laeuft PulseAudio, kann
 * dieser Aufruf fehlschlagen - oder im schlimmeren Fall das laufende Audio
 * stoeren. Daher zwei Stufen, per Argument gewaehlt:
 *
 *   --open-module   nur audio_hw_device_open (kein Stream, kein Routing)
 *   --open-stream   zusaetzlich einen Ausgabestream oeffnen
 *
 * Ohne Argument wird nur die Konfiguration gelesen und nichts geoeffnet.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <hardware/audio.h>

/* droid-util.h setzt voraus, dass diese Typen bereits bekannt sind -
 * in PulseAudio kommen sie ueber pulsecore/core.h herein. */
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

	printf("1) Konfiguration lesen ...\n");
	config = pa_parse_droid_audio_config("/android/vendor/etc/audio_policy_configuration.xml");
	if (!config) { fprintf(stderr, "   fehlgeschlagen\n"); return 1; }
	module = dm_config_find_module(config, "primary");
	printf("   OK: Modul \"%s\", %zd mixPorts\n", module->name,
			(ssize_t) dm_list_size(module->mix_ports));

	if (!open_module) {
		printf("\nHAL wird NICHT geoeffnet (kein --open-module angegeben).\n");
		dm_config_free(config);
		return 0;
	}

	printf("\n2) HAL oeffnen (audio_hw_device_open) ...\n");
	hw = pa_droid_hw_module_get(pa_compat_core(), config, "primary");
	if (!hw) {
		printf("   FEHLGESCHLAGEN - der HAL ist belegt oder nicht erreichbar.\n");
		printf("   Das ist die erwartete Antwort, solange PulseAudio laeuft:\n");
		printf("   der HAL waere damit exklusiv.\n");
		dm_config_free(config);
		return 2;
	}
	printf("   OK - HAL-Modul geoeffnet.\n");
	printf("\n   ACHTUNG, zwei getrennte Ebenen:\n");
	printf("   - Das HAL-Modul ist NICHT exklusiv: jeder Prozess bekommt eine\n");
	printf("     eigene audio_hw_device-Instanz.\n");
	printf("   - Das PCM-Geraet (/dev/snd/pcmC0D0p) ist es sehr wohl. Erscheint\n");
	printf("     oben \"pcm_hw_open: cannot open device\", haelt PulseAudio es\n");
	printf("     bereits - dann kommt trotz offenem HAL kein Ton heraus.\n");
	printf("   Ob wirklich gespielt werden kann, sagt erst --open-stream.\n");

	if (open_stream) {
		pa_droid_stream *s;
		/* Ports muessen aus hw->enabled_module kommen, nicht aus unserer
		 * Kopie - open_output_stream vergleicht per Zeigeridentitaet. */
		dm_config_port *mix = dm_config_find_mix_port(hw->enabled_module, "primary output");
		dm_config_port *dev = dm_config_default_output_device(hw->enabled_module);
		pa_sample_spec spec = { .format = PA_SAMPLE_S16LE, .rate = 48000, .channels = 2 };
		pa_channel_map map;
		pa_channel_map_init_stereo(&map);

		printf("\n3) Ausgabestream oeffnen (%s -> %s) ...\n",
				mix ? mix->name : "?", dev ? dev->name : "?");
		s = pa_droid_open_output_stream(hw, &spec, &map, mix, dev);
		if (!s) {
			printf("   fehlgeschlagen.\n");
		} else {
			printf("   OK - Stream offen.\n");
			pa_droid_stream_unref(s);
			printf("   Stream wieder geschlossen.\n");
		}
	}

	pa_droid_hw_module_unref(hw);
	dm_config_free(config);
	printf("\nFertig, alles wieder freigegeben.\n");
	return 0;
}
