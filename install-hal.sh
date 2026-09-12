#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Installs the SPA droid plugin and the PipeWire configuration for the pw-hal
# profile. Changes NOTHING about the running audio - afterwards 'standard' is
# still active. The switch only happens with: audioctl try pw-hal
set -e
cd "$(dirname "$0")"

SPA_DIR=/usr/lib/aarch64-linux-gnu/spa-0.2/droid
PLUGIN=poc/spa-droid/build/libspa-droid.so

[ -f "$PLUGIN" ] || { echo "plugin missing - build it first: ninja -C poc/spa-droid/build"; exit 1; }
command -v wireplumber >/dev/null || { echo "wireplumber missing - apt install wireplumber"; exit 1; }

echo "1) SPA plugin to $SPA_DIR"
sudo mkdir -p "$SPA_DIR"
sudo install -m644 "$PLUGIN" "$SPA_DIR/libspa-droid.so"

# Record the PipeWire version this was built against. If an update breaks the
# SPA interface, pw-hal would otherwise go silent without a word - audioctl
# now warns beforehand.
BUILT_AGAINST=$(pkg-config --modversion libpipewire-0.3 2>/dev/null || echo unknown)
echo "$BUILT_AGAINST" | sudo tee "$SPA_DIR/built-against" >/dev/null
echo "   built against PipeWire $BUILT_AGAINST"

echo "2) generating PipeWire configuration"
# mktemp, not a fixed name in /tmp: this file is installed into /usr with
# sudo, and a predictable path is one anyone on the machine can point
# somewhere else - or swap out between writing it and installing it.
HALCONF=$(mktemp) || { echo "could not create a temporary file" >&2; exit 1; }
trap 'rm -f "$HALCONF"' EXIT INT TERM
./gen-pipewire-hal-conf.py /usr/share/pipewire/pipewire-droid.conf "$HALCONF"
sudo mkdir -p /usr/local/share/furios-audio
sudo install -m644 "$HALCONF" /usr/local/share/furios-audio/pipewire-hal.conf
rm -f "$HALCONF"
trap - EXIT INT TERM

echo "3) WirePlumber monitor"
sudo mkdir -p /usr/local/share/wireplumber/scripts/monitors /usr/local/share/wireplumber/wireplumber.conf.d
sudo install -m644 wireplumber/droid.lua /usr/local/share/wireplumber/scripts/monitors/droid.lua
sudo install -m644 wireplumber/droid-input-follows-output.lua \
    /usr/local/share/wireplumber/scripts/monitors/droid-input-follows-output.lua
sudo install -m644 wireplumber/droid-default-sink-policy.lua \
    /usr/local/share/wireplumber/scripts/monitors/droid-default-sink-policy.lua
sudo install -m644 wireplumber/droid-bluetooth-call.lua \
    /usr/local/share/wireplumber/scripts/monitors/droid-bluetooth-call.lua
sudo install -m644 wireplumber/50-droid.conf /usr/local/share/wireplumber/wireplumber.conf.d/50-droid.conf
sudo install -m644 wireplumber/51-bluez-ofono.conf /usr/local/share/wireplumber/wireplumber.conf.d/51-bluez-ofono.conf

# /etc and not /usr/local: systemd does not look under /usr/local at all, so a
# drop-in placed there is simply never read. The package installs the same
# file under /usr/lib, which is where a package's drop-ins belong; /etc is the
# administrator's place and is what this script-driven install can use.
sudo mkdir -p /etc/systemd/system/ofono.service.d
sudo install -m644 systemd/ofono.service.d/30-furios-audio-hfp.conf \
    /etc/systemd/system/ofono.service.d/30-furios-audio-hfp.conf
sudo systemctl daemon-reload
# Not restarted here. ofono restarting takes the modem down for a moment, and
# on this device it has come back Powered but Online: false - no network and
# nothing on screen to say why. The drop-in takes effect at the next boot, or
# after "sudo systemctl restart ofono" when somebody is watching.

echo "4) echo experiment (DMNR)"
sudo install -m755 experiments/dmnr-handsfree.sh /usr/local/bin/furios-audio-dmnr

echo "5) updating audioctl (plugin path)"
sudo install -m755 audioctl /usr/local/bin/audioctl

echo
echo "Done. Active profile unchanged:"
audioctl status | head -4
echo
echo "Next step - a dry run with no risk:"
echo "   audioctl --dry-run set pw-hal"
echo "And then the real test (falls back to standard on reboot):"
echo "   audioctl try pw-hal"
