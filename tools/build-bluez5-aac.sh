#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Builds the AAC codec module that Debian's libspa-0.2-bluetooth leaves out.
#
# Why this exists
# ---------------
# Most earbuds offer AAC and SBC and nothing else. Without an AAC module
# PipeWire falls back to SBC - and the headset decides how good that can get:
# the soundcore Liberty 4 Pro caps SBC at bitpool 39, so even SBC-XQ runs into
# a ceiling. AAC is what such devices are tuned for.
#
# Debian omits the module because it needs fdk-aac, whose licence Debian keeps
# at arm's length. Building it locally is a different matter, and the result is
# purely additive: PipeWire loads codec modules one file at a time out of
# .../spa-0.2/bluez5/, so this drops one file in beside the others and replaces
# nothing that dpkg owns.
#
# The module is built against PipeWire's internal codec interface, so it has to
# be rebuilt after a PipeWire update. The version it was built against is
# recorded next to it, and audioctl warns when the two drift apart.
set -e

PWVER=$(pkg-config --modversion libpipewire-0.3)
SPADIR=$(pkg-config --variable=libdir libpipewire-0.3)/spa-0.2/bluez5
SRC=${SRC:-/tmp/pipewire-$PWVER-src}

echo "PipeWire $PWVER, module goes to $SPADIR"

MISSING=
for p in libfdk-aac-dev libdbus-1-dev libsbc-dev; do
    dpkg -s "$p" >/dev/null 2>&1 || MISSING="$MISSING $p"
done
for p in meson ninja git; do
    command -v "$p" >/dev/null 2>&1 || MISSING="$MISSING $p"
done
if [ -n "$MISSING" ]; then
    echo "missing build dependencies:$MISSING" >&2
    echo "  sudo apt install libfdk-aac-dev libdbus-1-dev libsbc-dev meson ninja-build git" >&2
    exit 1
fi

if [ ! -d "$SRC" ]; then
    echo "1) fetching the matching sources"
    git clone -q --depth 1 --branch "$PWVER" \
        https://gitlab.freedesktop.org/pipewire/pipewire.git "$SRC"
else
    echo "1) sources already in $SRC"
fi

echo "2) configuring - everything off except bluez5 and AAC"
rm -rf "$SRC/build-aac"
meson setup "$SRC/build-aac" "$SRC" \
    -Dbluez5=enabled -Dbluez5-codec-aac=enabled \
    -Dbluez5-codec-aptx=disabled -Dbluez5-codec-ldac=disabled \
    -Dbluez5-codec-lc3plus=disabled -Dbluez5-codec-opus=disabled \
    -Dbluez5-codec-lc3=disabled -Dbluez5-codec-g722=disabled \
    -Dalsa=disabled -Dpipewire-alsa=disabled -Dpipewire-jack=disabled -Djack=disabled \
    -Dv4l2=disabled -Dpipewire-v4l2=disabled -Dlibcamera=disabled -Dgstreamer=disabled \
    -Dlibsystemd=disabled -Dsystemd-system-service=disabled -Dsystemd-user-service=disabled \
    -Dtests=disabled -Dexamples=disabled -Dman=disabled -Ddocs=disabled -Dsdl2=disabled \
    -Dsndfile=disabled -Dpw-cat=disabled -Dvulkan=disabled -Dvolume=disabled \
    -Draop=disabled -Davahi=disabled -Decho-cancel-webrtc=disabled -Dlibpulse=disabled \
    -Dlibusb=disabled -Dudev=disabled -Dlibcanberra=disabled -Dcompress-offload=disabled \
    -Dx11=disabled -Dflatpak=disabled -Dreadline=disabled -Dgsettings=disabled \
    -Dlv2=disabled >/dev/null

echo "3) building the one module"
ninja -C "$SRC/build-aac" spa/plugins/bluez5/libspa-codec-bluez5-aac.so

echo "4) installing"
sudo install -m644 "$SRC/build-aac/spa/plugins/bluez5/libspa-codec-bluez5-aac.so" \
    "$SPADIR/libspa-codec-bluez5-aac.so"
printf '%s\n' "$PWVER" | sudo tee "$SPADIR/aac-built-against" >/dev/null

echo
echo "Done. Restart the audio stack and reconnect the headset:"
echo "   audioctl restart"
echo "   bluetoothctl disconnect <mac> && bluetoothctl connect <mac>"
echo "The card then offers \"High Fidelity Playback (A2DP Sink, codec AAC)\"."
