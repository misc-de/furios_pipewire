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

# Gegen die PipeWire-Version merken, gegen die gebaut wurde. Bricht ein
# Update die SPA-Schnittstelle, waere pw-hal sonst kommentarlos stumm -
# audioctl warnt jetzt vorher.
BUILT_AGAINST=$(pkg-config --modversion libpipewire-0.3 2>/dev/null || echo unbekannt)
echo "$BUILT_AGAINST" | sudo tee "$SPA_DIR/gebaut-gegen" >/dev/null
echo "   gebaut gegen PipeWire $BUILT_AGAINST"

echo "2) PipeWire-Konfiguration erzeugen"
./gen-pipewire-hal-conf.py /usr/share/pipewire/pipewire-droid.conf /tmp/pipewire-hal.conf
sudo mkdir -p /usr/local/share/furios-audio
sudo install -m644 /tmp/pipewire-hal.conf /usr/local/share/furios-audio/pipewire-hal.conf
rm -f /tmp/pipewire-hal.conf

echo "3) WirePlumber-Monitor"
sudo mkdir -p /usr/local/share/wireplumber/scripts/monitors /usr/local/share/wireplumber/wireplumber.conf.d
sudo install -m644 wireplumber/droid.lua /usr/local/share/wireplumber/scripts/monitors/droid.lua
sudo install -m644 wireplumber/50-droid.conf /usr/local/share/wireplumber/wireplumber.conf.d/50-droid.conf
sudo install -m644 wireplumber/51-bluez-ofono.conf /usr/local/share/wireplumber/wireplumber.conf.d/51-bluez-ofono.conf

echo "4) audioctl aktualisieren (Plugin-Pfad)"
sudo install -m755 audioctl /usr/local/bin/audioctl

echo
echo "Fertig. Aktives Profil unveraendert:"
audioctl status | head -4
echo
echo "Naechster Schritt - Probelauf ohne Risiko:"
echo "   audioctl --dry-run set pw-hal"
echo "Und dann der echte Test (faellt beim Neustart zurueck auf standard):"
echo "   audioctl try pw-hal"
