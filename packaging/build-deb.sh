#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Builds a .deb from the work tree.
#
# Without debhelper - dpkg-deb is enough for a package of this size, and a
# hand-written dependency list is more honest than any automation: it names
# exactly the PipeWire version the plugin was built against.
set -e
cd "$(dirname "$0")/.."
ROOT=$(pwd)

PLUGIN=poc/spa-droid/build/libspa-droid.so
[ -f "$PLUGIN" ] || { echo "plugin missing - build it first: ninja -C poc/spa-droid/build"; exit 1; }

ARCH=$(dpkg --print-architecture)
TRIPLET=$(dpkg-architecture -qDEB_HOST_MULTIARCH)
PWVER=$(pkg-config --modversion libpipewire-0.3 2>/dev/null || echo 1.6.6)
VERSION="0.1.0+git$(git log -1 --format=%cd --date=format:%Y%m%d 2>/dev/null || date +%Y%m%d).$(git rev-parse --short HEAD 2>/dev/null || echo 0)"
# Uncommitted changes get their own number - otherwise the package would carry
# the version of the last commit and dpkg would consider it the same.
[ -n "$(git status --porcelain 2>/dev/null)" ] && VERSION="$VERSION+dirty$(date +%H%M%S)"
PKG="furios-audio-pipewire"
STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT

echo "package $PKG $VERSION ($ARCH), built against PipeWire $PWVER"

# --- files ---
install -Dm644 "$PLUGIN" "$STAGE/usr/lib/$TRIPLET/spa-0.2/droid/libspa-droid.so"
printf '%s\n' "$PWVER" > "$STAGE/usr/lib/$TRIPLET/spa-0.2/droid/built-against"

install -Dm755 audioctl                       "$STAGE/usr/bin/audioctl"
install -Dm755 gui/furios-audio-switch.py     "$STAGE/usr/bin/furios-audio-switch"
install -Dm755 experiments/dmnr-handsfree.sh  "$STAGE/usr/bin/furios-audio-dmnr"

install -Dm644 tunnel.conf                    "$STAGE/usr/share/furios-audio/tunnel.conf"
./gen-pipewire-hal-conf.py /usr/share/pipewire/pipewire-droid.conf "$STAGE/tmp-hal.conf" >/dev/null
install -Dm644 "$STAGE/tmp-hal.conf"          "$STAGE/usr/share/furios-audio/pipewire-hal.conf"
rm -f "$STAGE/tmp-hal.conf"

install -Dm644 wireplumber/droid.lua          "$STAGE/usr/share/wireplumber/scripts/monitors/droid.lua"
install -Dm644 wireplumber/droid-input-follows-output.lua \
    "$STAGE/usr/share/wireplumber/scripts/monitors/droid-input-follows-output.lua"
install -Dm644 wireplumber/50-droid.conf      "$STAGE/usr/share/wireplumber/wireplumber.conf.d/50-droid.conf"
install -Dm644 wireplumber/51-bluez-ofono.conf "$STAGE/usr/share/wireplumber/wireplumber.conf.d/51-bluez-ofono.conf"

install -Dm644 furios-pw-tunnel.service       "$STAGE/usr/lib/systemd/user/furios-pw-tunnel.service"
install -Dm644 furios-audio-apply.service     "$STAGE/usr/lib/systemd/user/furios-audio-apply.service"

install -Dm644 gui/de.furios.audioswitch.desktop \
    "$STAGE/usr/share/applications/de.furios.audioswitch.desktop"
install -Dm644 gui/de.furios.audioswitch.svg \
    "$STAGE/usr/share/icons/hicolor/scalable/apps/de.furios.audioswitch.svg"
install -Dm644 README.md "$STAGE/usr/share/doc/$PKG/README.md"

# The sources here are MIT, but the plugin links LGPL-2.1 code from
# pulseaudio-modules-droid-modern - so the package as a whole is LGPL-2.1.
# Debian expects that stated in the copyright file, not in the control file.
install -Dm644 LICENSE "$STAGE/usr/share/doc/$PKG/LICENSE"
cat > "$STAGE/usr/share/doc/$PKG/copyright" <<'COPY'
Format: https://www.debian.org/doc/packaging-manuals/copyright-format/1.0/
Upstream-Name: furios_pipewire
Source: https://github.com/misc-de/furios_pipewire

Files: *
Copyright: 2026 misc-de
License: MIT

Files: libspa-droid.so
Copyright: 2026 misc-de
           2013-2022 Jolla Ltd.
Comment: The plugin is built from the MIT sources of this project together
 with source files of pulseaudio-modules-droid-modern (droid-util.c,
 droid-config.c, config-parser-xml.c, conversion.c, sllist.c, utils.c).
 The binary is a combined work and is distributed under the LGPL-2.1.
License: LGPL-2.1

License: MIT
 Permission is hereby granted, free of charge, to any person obtaining a
 copy of this software and associated documentation files (the "Software"),
 to deal in the Software without restriction, including without limitation
 the rights to use, copy, modify, merge, publish, distribute, sublicense,
 and/or sell copies of the Software, and to permit persons to whom the
 Software is furnished to do so, subject to the following conditions:
 .
 The above copyright notice and this permission notice shall be included
 in all copies or substantial portions of the Software.
 .
 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

License: LGPL-2.1
 This library is free software; you can redistribute it and/or modify it
 under the terms of the GNU Lesser General Public License as published by
 the Free Software Foundation, version 2.1 of the License.
 .
 This library is distributed in the hope that it will be useful, but
 WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser
 General Public License for more details.
 .
 On Debian systems the full text is in
 /usr/share/common-licenses/LGPL-2.1.
COPY
chmod 644 "$STAGE/usr/share/doc/$PKG/copyright"

# --- control file ---
mkdir -p "$STAGE/DEBIAN"
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Architecture: $ARCH
Maintainer: misc-de <11610690+misc-de@users.noreply.github.com>
Section: sound
Priority: optional
Depends: pipewire (>= $PWVER), pipewire (<< $(echo "$PWVER" | cut -d. -f1).$(( $(echo "$PWVER" | cut -d. -f2) + 1 ))),
 pipewire-pulse, wireplumber, pulseaudio, libhardware2, libexpat1,
 python3-gi, gir1.2-adw-1, gir1.2-gtk-4.0, pulseaudio-utils
Recommends: libspa-0.2-bluetooth
Description: PipeWire talks directly to the Android audio HAL
 An SPA plugin that connects the Android audio HAL (libhybris) directly to
 PipeWire - with no PulseAudio in between. Playback, capture and telephony all
 run through it, including earpiece/speaker switching during a call.
 .
 Ships audioctl for switching between the shipped state and the new stack
 (reversible at any time, with a safety net) plus a small GTK4 front end for
 it.
 .
 The version bound on pipewire is deliberate: the plugin is built against a
 specific SPA interface. If an update breaks it, the sound would otherwise
 disappear without a word.
EOF

cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
set -e
# State directory; the switcher writes into it as the user.
mkdir -p /var/lib/furios-audio
chmod 1777 /var/lib/furios-audio
[ -e /var/lib/furios-audio/profile ] || echo standard > /var/lib/furios-audio/profile
# The file is created by root, but audioctl runs as the user and rewrites it on
# every switch - without this it could never persist a profile.
chmod 666 /var/lib/furios-audio/profile
gtk-update-icon-cache -qtf /usr/share/icons/hicolor 2>/dev/null || true
update-desktop-database -q /usr/share/applications 2>/dev/null || true
echo "Installed. Active profile unchanged - switch with: audioctl toggle"
EOF
chmod 755 "$STAGE/DEBIAN/postinst"

cat > "$STAGE/DEBIAN/prerm" <<'EOF'
#!/bin/sh
set -e
# Anyone still on pw-hal at removal time would end up with no sound: the
# plugin disappears while the configuration still points at it.
if [ "$1" = remove ] && [ "$(cat /var/lib/furios-audio/profile 2>/dev/null)" = pw-hal ]; then
    echo "WARNING: profile pw-hal is set persistently." >&2
    echo "         Run 'audioctl set standard' before removing," >&2
    echo "         otherwise the sound stays gone after the next reboot." >&2
fi
exit 0
EOF
chmod 755 "$STAGE/DEBIAN/prerm"

# Clear out older packages: a "dpkg -i packaging/*.deb" would otherwise
# install all of them - and in glob order possibly the oldest one last. That
# happened once and looked exactly like a broken plugin.
rm -f "$ROOT/packaging/${PKG}_"*.deb

OUT="$ROOT/packaging/${PKG}_${VERSION}_${ARCH}.deb"
dpkg-deb --root-owner-group --build "$STAGE" "$OUT" >/dev/null
echo "done: $OUT"

if [ "${1:-}" = --install ]; then
    sudo dpkg -i "$OUT"
fi
