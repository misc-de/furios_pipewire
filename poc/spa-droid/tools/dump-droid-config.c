/* SPDX-FileCopyrightText: Copyright (c) 2026 misc-de */
/* SPDX-License-Identifier: MIT */
/* Verifies the PulseAudio-free port: reads the device's real audio_policy XML
 * with the ported parser and prints what the HAL offers. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <hardware/audio.h>
#include "droid/droid-config.h"
#include "droid/conversion.h"
#include "droid/sllist.h"

static const char *role_str(dm_config_role_t r) {
    return r == DM_CONFIG_ROLE_SINK ? "sink" : "source";
}

static void print_port(dm_config_port *p) {
    const char *devname = NULL, *paname = NULL;
    if (p->port_type == DM_CONFIG_TYPE_DEVICE_PORT) {
        if (p->role == DM_CONFIG_ROLE_SINK) {
            pa_string_convert_output_device_num_to_str(p->type, &devname);
            pa_droid_output_port_name(p->type, &paname);
        } else {
            pa_string_convert_input_device_num_to_str(p->type, &devname);
            pa_droid_input_port_name(p->type, &paname);
        }
        printf("      %-34s %-7s type=%#010x %-38s -> %s\n",
               p->name, role_str(p->role), p->type,
               devname ? devname : "(unknown)", paname ? paname : "(no PA port)");
    } else {
        char *flags = pa_list_string_flags(p->flags);
        printf("      %-34s %-7s flags=%#x %s\n",
               p->name, role_str(p->role), p->flags, flags ? flags : "");
        free(flags);
    }
}

int main(int argc, char **argv) {
    const char *fn = argc > 1 ? argv[1] : "/android/vendor/etc/audio_policy_configuration.xml";
    dm_config_device *config;
    dm_config_module *m;
    void *mstate = NULL;

    printf("Datei: %s\n\n", fn);
    if (!(config = pa_parse_droid_audio_config(fn))) {
        fprintf(stderr, "Parsen fehlgeschlagen.\n");
        return 1;
    }

    for (m = dm_list_first_data(config->modules, &mstate); m;
         m = dm_list_next_data(config->modules, &mstate)) {
        void *s = NULL;
        dm_config_port *p;
        printf("Modul: %s (HAL-Version %d.%d)\n", m->name, m->version_major, m->version_minor);
        printf("  Standard-Ausgabegeraet: %s\n",
               m->default_output_device ? m->default_output_device->name : "(keins)");

        printf("  mixPorts (%zd):\n", (ssize_t) dm_list_size(m->mix_ports));
        for (p = dm_list_first_data(m->mix_ports, &s); p; p = dm_list_next_data(m->mix_ports, &s))
            print_port(p);

        s = NULL;
        printf("  devicePorts (%zd):\n", (ssize_t) dm_list_size(m->device_ports));
        for (p = dm_list_first_data(m->device_ports, &s); p; p = dm_list_next_data(m->device_ports, &s))
            print_port(p);

        printf("  Routen: %zd\n\n", (ssize_t) dm_list_size(m->routes));
    }

    dm_config_free(config);
    return 0;
}
