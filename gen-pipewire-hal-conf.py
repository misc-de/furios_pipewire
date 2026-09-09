#!/usr/bin/env python3
"""Erzeugt pipewire-hal.conf aus der FuriOS-Datei pipewire-droid.conf.

Statt einer Kopie werden nur zwei Dinge eingefuegt:
  1. api.droid.* -> unser SPA-Plugin in context.spa-libs
  (Geraet und Knoten legt WirePlumber an, siehe wireplumber/droid.lua)

Damit wandern spaetere FuriOS-Aenderungen an pipewire-droid.conf mit,
sobald man dieses Skript erneut laufen laesst.
"""
import re
import sys

NODE = ""   # Geraet und Knoten kommen von WirePlumber (droid.lua)


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

    # 2) Device anhaengen
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
