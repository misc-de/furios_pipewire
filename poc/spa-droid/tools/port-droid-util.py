#!/usr/bin/env python3
"""Erzeugt aus dem unveraenderten droid-util.c eine SPA-taugliche Fassung.

Ausgeschlossen wird ausschliesslich Code, der PulseAudio-Graphobjekte
(pa_sink, pa_card, Hooks) dereferenziert. Diese Funktionen haben im SPA-Plugin
kein Gegenstueck - dort uebernimmt das SPA-Device die Port-/Profilverwaltung.

Reproduzierbar: laesst sich nach einem Upstream-Update erneut ausfuehren.
"""
import re
import sys

# Ganze Funktionen, die PA-Graphobjekte dereferenzieren.
EXCLUDE_FUNCS = [
    "add_ports",                  # legt pa_device_port an
    "pa_droid_add_ports",         # card->core, card->ports
    "pa_droid_add_card_ports",    # pa_card_profile
    "update_sink_types",          # sink->...
    "sink_put_hook_cb",           # PA-Hook-Callback
    "sink_unlink_hook_cb",        # PA-Hook-Callback
    "pa_source_is_droid_source",  # source->proplist
    "pa_sink_is_droid_sink",      # sink->proplist
]

# Einzelanweisungen innerhalb von Funktionen, die wir behalten.
REPLACEMENTS = [
    (
        """    hw->sink_put_hook_slot      = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_PUT], PA_HOOK_EARLY-10,
                                                  sink_put_hook_cb, hw);
    hw->sink_unlink_hook_slot   = pa_hook_connect(&core->hooks[PA_CORE_HOOK_SINK_UNLINK], PA_HOOK_EARLY-10,
                                                  sink_unlink_hook_cb, hw);
""",
        """    /* SPA-Port: PulseAudio-Sink-Hooks entfallen; die Sink-Typ-Pflege
     * macht im SPA-Plugin das Device selbst. */
    hw->sink_put_hook_slot = NULL;
    hw->sink_unlink_hook_slot = NULL;
""",
    ),
    (
        """    if (hw->sink_put_hook_slot)
        pa_hook_slot_free(hw->sink_put_hook_slot);
    if (hw->sink_unlink_hook_slot)
        pa_hook_slot_free(hw->sink_unlink_hook_slot);
""",
        """    /* SPA-Port: keine Hook-Slots zu loesen. */
""",
    ),
]


def exclude_function(text, name):
    """Klammert die Definition von *name* in #if 0 ... #endif ein."""
    # Definitionszeile: beginnt in Spalte 0, enthaelt name( und endet mit {
    pattern = re.compile(
        r"^((?:[A-Za-z_][\w \t\*]*?)\b" + re.escape(name) + r"\s*\([^;]*?\)\s*\{)$",
        re.MULTILINE,
    )
    m = pattern.search(text)
    if not m:
        return text, False
    start = m.start()
    # Funktionsende: naechste Zeile, die exakt "}" ist
    end = text.index("\n}\n", m.end()) + len("\n}\n")
    body = text[start:end]
    guarded = (
        "#if 0 /* SPA-Port: dereferenziert PulseAudio-Graphobjekte */\n"
        + body
        + "#endif\n"
    )
    return text[:start] + guarded + text[end:], True


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src).read()

    missing = []
    for name in EXCLUDE_FUNCS:
        text, ok = exclude_function(text, name)
        if not ok:
            missing.append(name)

    for old, new in REPLACEMENTS:
        if old not in text:
            missing.append("<Anweisungsblock>")
            continue
        text = text.replace(old, new, 1)

    if missing:
        print("FEHLER: nicht gefunden: %s" % ", ".join(missing), file=sys.stderr)
        print("Upstream hat sich geaendert - EXCLUDE_FUNCS/REPLACEMENTS pruefen.",
              file=sys.stderr)
        return 1

    header = (
        "/* Erzeugt von tools/port-droid-util.py aus %s\n"
        " * NICHT von Hand bearbeiten. */\n" % src
    )
    open(dst, "w").write(header + text)
    print("%s -> %s (%d Funktionen ausgeschlossen)" % (src, dst, len(EXCLUDE_FUNCS)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
