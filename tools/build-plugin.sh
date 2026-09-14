#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# The SPA plugin, from a fresh clone to a built libspa-droid.so.
#
# This exists because install.sh installed half a stack: audioctl and the
# units, and nothing that makes PipeWire talk to the HAL. The other half was
# four commands under "Building" in the README - and somebody who presses
# "Install" in the app never sees a README. Now the one script does the whole
# thing.
#
# Everything here looks before it acts, so a second run costs a second and
# changes nothing.
set -e
cd "$(dirname "$0")/.."

# What the build needs, by package name. Checked by name rather than by
# building and reading the error: meson's message for a missing header is a
# page long and names a header, never a package.
PAKETE=(meson ninja-build build-essential pkg-config libexpat1-dev
        libspa-0.2-dev libpulse-dev libhybris-common-dev libhardware-dev
        libbluetooth-dev android-headers-30)

# The upstream droid sources are LGPL-2.1 and are not versioned in this repo
# (see NOTICE). The commit is pinned: it is the one the port script's patterns
# were written against, and an unpinned clone turns "it built yesterday" into
# a question about somebody else's day.
UPSTREAM=https://github.com/FuriLabs/pulseaudio-modules-droid-modern
COMMIT=d0e2330
QUELLEN=src/pulseaudio-modules-droid-modern
BUILD=poc/spa-droid/build
PLUGIN=$BUILD/libspa-droid.so

echo "   packages the build needs"
fehlt=()
for p in "${PAKETE[@]}"; do
    if ! dpkg-query -W -f='${Status}' "$p" 2>/dev/null | grep -q "^install ok installed$"; then
        fehlt+=("$p")
    fi
done
if [ ${#fehlt[@]} -eq 0 ]; then
    echo "      all present"
else
    echo "      installing ${fehlt[*]}"
    # A stale package index is the ordinary reason the first attempt fails on
    # a phone that has not seen "apt update" in weeks, so try again after one.
    sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${fehlt[@]}" \
        || { sudo apt-get update
             sudo env DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends "${fehlt[@]}"; }
fi

echo "   upstream droid sources"
if [ -d "$QUELLEN/src/common" ]; then
    echo "      already here - left exactly as they are"
else
    git clone "$UPSTREAM" "$QUELLEN"
    git -C "$QUELLEN" checkout --quiet "$COMMIT"
    echo "      cloned at $COMMIT"
fi

echo "   compiling"
[ -d "$BUILD" ] || meson setup "$BUILD" poc/spa-droid
ninja -C "$BUILD"
[ -f "$PLUGIN" ] || { echo "the build produced no $PLUGIN" >&2; exit 1; }
echo "      $PLUGIN"
