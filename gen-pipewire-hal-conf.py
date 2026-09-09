#!/usr/bin/env python3
"""Erzeugt pipewire-hal.conf aus der FuriOS-Datei pipewire-droid.conf.

Statt einer Kopie werden nur zwei Dinge eingefuegt:
  1. api.droid.* -> unser SPA-Plugin in context.spa-libs
  2. je ein Sink- und ein Source-Knoten in context.objects

Damit wandern spaetere FuriOS-Aenderungen an pipewire-droid.conf mit,
sobald man dieses Skript erneut laufen laesst.
"""
import re
import sys

NODE = """    { factory = adapter
        args = {
            factory.name        = api.droid.pcm
            node.name           = droid-sink
            node.description    = "Android HAL (Wiedergabe)"
            media.class         = "Audio/Sink"
            droid.mix-port      = "primary output"
            audio.format        = "S16LE"
            audio.rate          = 48000
            audio.channels      = 2
            audio.position      = "FL,FR"
            node.driver         = true
            priority.driver     = 50000
            priority.session    = 1000
        }
    }
    { factory = adapter
        args = {
            factory.name        = api.droid.pcm.source
            node.name           = droid-source
            node.description    = "Android HAL (Aufnahme)"
            media.class         = "Audio/Source"
            droid.mix-port      = "primary input"
            audio.format        = "S16LE"
            audio.rate          = 48000
            audio.channels      = 2
            audio.position      = "FL,FR"
            # Auch die Aufnahme taktet ihren Graphen selbst, aber mit
            # niedrigerer Prioritaet: sind Sink und Source verbunden,
            # soll die Wiedergabe den Takt vorgeben.
            node.driver         = true
            priority.driver     = 20000
            priority.session    = 1000
        }
    }
"""


def main():
    src, dst = sys.argv[1], sys.argv[2]
    text = open(src).read()

    # 1) SPA-Bibliothek registrieren
    if "api.droid." in text:
        print("Hinweis: api.droid.* steht bereits in der Vorlage", file=sys.stderr)
    else:
        m = re.search(r"^context\.spa-libs\s*=\s*\{\s*$", text, re.M)
        if not m:
            print("FEHLER: context.spa-libs nicht gefunden", file=sys.stderr)
            return 1
        insert = m.end() + 1
        text = (text[:insert]
                + "    api.droid.*     = droid/libspa-droid\n"
                + text[insert:])

    # 2) Sink- und Source-Knoten anhaengen
    m = re.search(r"^context\.objects\s*=\s*\[\s*$", text, re.M)
    if not m:
        print("FEHLER: context.objects nicht gefunden", file=sys.stderr)
        return 1
    insert = m.end() + 1
    text = text[:insert] + NODE + text[insert:]

    header = ("# Erzeugt von gen-pipewire-hal-conf.py aus %s\n"
              "# NICHT von Hand bearbeiten - Skript erneut ausfuehren.\n" % src)
    open(dst, "w").write(header + text)
    print("%s -> %s" % (src, dst))
    return 0


if __name__ == "__main__":
    sys.exit(main())
