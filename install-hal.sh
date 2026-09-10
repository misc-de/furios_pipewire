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
./gen-pipewire-hal-conf.py /usr/share/pipewire/pipewire-droid.conf /tmp/pipewire-hal.conf
sudo mkdir -p /usr/local/share/furios-audio
sudo install -m644 /tmp/pipewire-hal.conf /usr/local/share/furios-audio/pipewire-hal.conf
rm -f /tmp/pipewire-hal.conf

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
