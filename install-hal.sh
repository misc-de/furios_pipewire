#!/bin/bash
# Installiert das SPA-Droid-Plugin und die PipeWire-Konfiguration fuer das
# Profil pw-hal. Aendert NICHTS am laufenden Audio - danach ist weiterhin
# 'standard' aktiv. Der Wechsel passiert erst mit: audioctl try pw-hal
set -e
cd "$(dirname "$0")"

SPA_DIR=/usr/lib/aarch64-linux-gnu/spa-0.2/droid
PLUGIN=poc/spa-droid/build/libspa-droid.so

[ -f "$PLUGIN" ] || { echo "Plugin fehlt - erst bauen: ninja -C poc/spa-droid/build"; exit 1; }
command -v wireplumber >/dev/null || { echo "wireplumber fehlt - apt install wireplumber"; exit 1; }

echo "1) SPA-Plugin nach $SPA_DIR"
sudo mkdir -p "$SPA_DIR"
sudo install -m644 "$PLUGIN" "$SPA_DIR/libspa-droid.so"

echo "2) PipeWire-Konfiguration erzeugen"
./gen-pipewire-hal-conf.py /usr/share/pipewire/pipewire-droid.conf /tmp/pipewire-hal.conf
sudo mkdir -p /usr/local/share/furios-audio
sudo install -m644 /tmp/pipewire-hal.conf /usr/local/share/furios-audio/pipewire-hal.conf
sudo install -m644 pipewire-hal.conf.dropin /usr/local/share/furios-audio/pipewire-hal.conf.dropin
rm -f /tmp/pipewire-hal.conf

echo "3) audioctl aktualisieren (Plugin-Pfad)"
sudo install -m755 audioctl /usr/local/bin/audioctl

echo
echo "Fertig. Aktives Profil unveraendert:"
audioctl status | head -4
echo
echo "Naechster Schritt - Probelauf ohne Risiko:"
echo "   audioctl --dry-run set pw-hal"
echo "Und dann der echte Test (faellt beim Neustart zurueck auf standard):"
echo "   audioctl try pw-hal"
