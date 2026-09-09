#!/bin/bash
# Entfernt alles restlos und stellt den Auslieferungszustand her.
set -e
/usr/local/bin/audioctl revert 2>/dev/null || true
systemctl --user disable --now furios-audio-apply.service furios-pw-tunnel.service 2>/dev/null || true
sudo rm -f /usr/local/bin/audioctl \
           /etc/systemd/user/furios-pw-tunnel.service \
           /etc/systemd/user/furios-audio-apply.service \
           /etc/systemd/user/pipewire.service.d/50-furios-audio.conf
sudo rm -rf /usr/local/share/furios-audio /var/lib/furios-audio
sudo rm -rf /usr/lib/aarch64-linux-gnu/spa-0.2/droid
sudo rmdir /etc/systemd/user/pipewire.service.d 2>/dev/null || true
# Umschalter-App
sudo rm -f /usr/local/bin/furios-audio-switch \
           /usr/local/share/applications/de.furios.audioswitch.desktop \
           /usr/local/share/icons/hicolor/scalable/apps/de.furios.audioswitch.svg

# WirePlumber-Monitor und Bluetooth-Konfiguration
sudo rm -f /usr/local/share/wireplumber/scripts/monitors/droid.lua \
           /usr/local/share/wireplumber/wireplumber.conf.d/50-droid.conf \
           /usr/local/share/wireplumber/wireplumber.conf.d/51-bluez-ofono.conf
sudo rmdir --ignore-fail-on-non-empty \
    /usr/local/share/wireplumber/scripts/monitors \
    /usr/local/share/wireplumber/scripts \
    /usr/local/share/wireplumber/wireplumber.conf.d \
    /usr/local/share/wireplumber 2>/dev/null || true
# FuriOS-Masken wiederherstellen
for u in pipewire-pulse.service pipewire-pulse.socket wireplumber.service; do sudo ln -sf /dev/null "/etc/systemd/user/$u"; done
for u in pulseaudio.service pulseaudio.socket; do
  [ "$(readlink "/etc/systemd/user/$u" 2>/dev/null)" = /dev/null ] && sudo rm -f "/etc/systemd/user/$u"
done
systemctl --user daemon-reload
systemctl --user start pulseaudio.socket pulseaudio.service 2>/dev/null || true
echo "Auslieferungszustand wiederhergestellt."
