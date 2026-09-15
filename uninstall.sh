#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Removes everything and restores the shipped state.
set -e
/usr/local/bin/audioctl revert 2>/dev/null || true
systemctl --user disable --now furios-audio-apply.service furios-audio-verify.service \
    furios-pw-tunnel.service furios-audio-pause-on-disconnect.service \
    furios-audio-callaudio-refresh.service furios-audio-sco-hold.service \
    furios-audio-bt-mic.service furios-audio-bt-reconnect.service \
    2>/dev/null || true
# The echo suppression first, and in this order: take the mount down and drop
# the marker while the tool is still there to do it. Removing the binary first
# would leave a marker nothing reads and a mount nothing undoes.
sudo systemctl disable --now furios-audio-dmnr.service >/dev/null 2>&1 || true
[ -x /usr/local/bin/furios-audio-dmnr ] && \
    /usr/local/bin/furios-audio-dmnr set off >/dev/null 2>&1 || true
sudo rm -f /etc/systemd/system/furios-audio-dmnr.service \
           /etc/furios-audio-dmnr.persistent

sudo rm -f /usr/local/bin/audioctl \
           /usr/local/bin/furios-audio-dmnr \
           /usr/local/bin/furios-audio-pause-on-disconnect \
           /usr/local/bin/furios-audio-callaudio-refresh \
           /usr/local/bin/furios-audio-sco-hold \
           /usr/local/bin/furios-audio-bt-mic \
           /usr/local/bin/furios-audio-bt-reconnect \
           /etc/systemd/user/furios-pw-tunnel.service \
           /etc/systemd/user/furios-audio-apply.service \
           /etc/systemd/user/furios-audio-verify.service \
           /etc/systemd/user/furios-audio-pause-on-disconnect.service \
           /etc/systemd/user/furios-audio-callaudio-refresh.service \
           /etc/systemd/user/furios-audio-sco-hold.service \
           /etc/systemd/user/furios-audio-bt-mic.service \
           /etc/systemd/user/furios-audio-bt-reconnect.service \
           /etc/systemd/user/pipewire.service.d/50-furios-audio.conf
sudo rm -rf /usr/local/share/furios-audio /var/lib/furios-audio
sudo rm -rf /usr/lib/aarch64-linux-gnu/spa-0.2/droid
sudo rmdir /etc/systemd/user/pipewire.service.d 2>/dev/null || true
# switcher app
# Both names: the app was called furios-audio-switch until it grew a second
# page, and an uninstall that only knows the new name leaves the old launcher
# in the app grid pointing at a program that is gone.
sudo rm -f /usr/local/bin/misc-de \
           /usr/local/share/applications/de.misc-de.tools.desktop \
           /usr/local/share/icons/hicolor/scalable/apps/de.misc-de.tools.svg \
           /usr/local/bin/furios-audio-switch \
           /usr/local/share/applications/de.furios.audioswitch.desktop \
           /usr/local/share/icons/hicolor/scalable/apps/de.furios.audioswitch.svg

# WirePlumber monitor and Bluetooth configuration
sudo rm -f /usr/local/share/wireplumber/scripts/monitors/droid.lua \
           /usr/local/share/wireplumber/scripts/monitors/droid-input-follows-output.lua \
           /usr/local/share/wireplumber/scripts/monitors/droid-default-sink-policy.lua \
           /usr/local/share/wireplumber/scripts/monitors/droid-bluetooth-call.lua \
           /usr/local/share/wireplumber/wireplumber.conf.d/50-droid.conf \
           /usr/local/share/wireplumber/wireplumber.conf.d/51-bluez-ofono.conf
sudo rm -f /etc/systemd/system/ofono.service.d/30-furios-audio-hfp.conf
sudo rmdir /etc/systemd/system/ofono.service.d 2>/dev/null || true
sudo rmdir --ignore-fail-on-non-empty \
    /usr/local/share/wireplumber/scripts/monitors \
    /usr/local/share/wireplumber/scripts \
    /usr/local/share/wireplumber/wireplumber.conf.d \
    /usr/local/share/wireplumber 2>/dev/null || true
# restore the FuriOS masks
for u in pipewire-pulse.service pipewire-pulse.socket wireplumber.service; do sudo ln -sf /dev/null "/etc/systemd/user/$u"; done
for u in pulseaudio.service pulseaudio.socket; do
  [ "$(readlink "/etc/systemd/user/$u" 2>/dev/null)" = /dev/null ] && sudo rm -f "/etc/systemd/user/$u"
done
systemctl --user daemon-reload
systemctl --user start pulseaudio.socket pulseaudio.service 2>/dev/null || true
echo "Shipped state restored."
