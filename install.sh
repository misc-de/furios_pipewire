#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Installs the whole stack: the SPA plugin (built here if it is not built
# yet), the WirePlumber monitor, audioctl and every unit audioctl knows about.
#
# Changes NOTHING about the running audio - afterwards the shipped profile is
# still active and the switch is one press away.
#
# It used to install half of this: audioctl and three units, while the plugin
# that makes PipeWire talk to the HAL was four commands in the README. A phone
# where somebody pressed Install got a switch with nothing behind it.
set -e

# Not with sudo, and this has to be caught here rather than halfway through.
# Every line below that needs root asks for it itself, and two of them must NOT
# have it: "systemctl --user" reaches the session bus through $XDG_RUNTIME_DIR,
# which root does not have, and the chown further down takes its owner from
# "id -un" - as root that is root, and the state directory would end up owned
# by a user audioctl never runs as. Started with sudo this used to build the
# plugin, copy half the files and then stop at the first --user call, leaving
# the units in place but never reloaded.
if [ "$(id -u)" = 0 ]; then
    echo "Please run this WITHOUT sudo - it asks for root where it needs it." >&2
    echo "  ./install.sh" >&2
    exit 1
fi
cd "$(dirname "$0")"

# The four newer units name /usr/bin, because that is where the package puts
# their programs. This install goes to /usr/local, so the path is rewritten on
# the way in - the same thing the modem and GPS installers do. The two older
# ones (apply, verify) look in both places themselves and are copied as they
# are.
# Unit file, then the program it starts. The unit is named in full rather
# than built from the program name: a name that only exists as a shell
# variable cannot be grepped for, and tests/test-install.sh compares these
# three scripts by reading them.
WERKZEUGE=(
    "furios-audio-pause-on-disconnect.service tools/furios-audio-pause-on-disconnect.py"
    "furios-audio-callaudio-refresh.service   tools/furios-audio-callaudio-refresh"
    "furios-audio-sco-hold.service            tools/furios-audio-sco-hold.py"
    "furios-audio-bt-mic.service              tools/furios-audio-bt-mic.py"
    "furios-audio-bt-reconnect.service        tools/furios-audio-bt-reconnect.py"
)

echo "== the plugin PipeWire needs to reach the HAL"
./tools/build-plugin.sh

echo "== audioctl, its units and its state"
sudo mkdir -p /usr/local/share/furios-audio /usr/local/bin
sudo install -m755 audioctl                  /usr/local/bin/audioctl
sudo install -m644 tunnel.conf               /usr/local/share/furios-audio/tunnel.conf
sudo install -m644 furios-pw-tunnel.service  /etc/systemd/user/furios-pw-tunnel.service
sudo install -m644 furios-audio-apply.service /etc/systemd/user/furios-audio-apply.service
sudo install -m644 furios-audio-verify.service /etc/systemd/user/furios-audio-verify.service

# Bluetooth calls, Bluetooth microphone, the podcast that must not resume in
# somebody's pocket, and the routing refresh callaudiod needs. audioctl
# enables each of them itself when a profile is applied - but only if the file
# is here, and until now it never was on a script install.
for eintrag in "${WERKZEUGE[@]}"; do
    # shellcheck disable=SC2086
    set -- $eintrag
    unit=$1
    quelle=$2
    sudo install -m755 "$quelle" "/usr/local/bin/${unit%.service}"
    sed "s|^ExecStart=/usr/bin/|ExecStart=/usr/local/bin/|" "$unit" \
        | sudo tee "/etc/systemd/user/$unit" >/dev/null
    sudo chmod 644 "/etc/systemd/user/$unit"
done

# The state directory has to be WRITABLE by this user, not merely present:
# "mkdir -p" succeeds on a directory that already exists and belongs to root,
# and then the write below fails and takes the whole install down with it.
sudo mkdir -p /var/lib/furios-audio
[ -w /var/lib/furios-audio ] || sudo chown "$(id -un):$(id -gn)" /var/lib/furios-audio
sudo chmod 0755 /var/lib/furios-audio
# Only if there is nothing yet. This used to be written unconditionally, so
# re-running install.sh threw away a profile somebody had set persistently -
# and the phone went back to the shipped stack at the next boot without anyone
# asking for it. The package's postinst has always done it this way.
[ -e /var/lib/furios-audio/profile ] || echo standard > /var/lib/furios-audio/profile
# An older, root-based audioctl put its masks in /etc/systemd/user. systemd
# reads ~/.config/systemd/user first, but a mask in /etc goes on masking, and
# this version has no way to remove it - so the first switch after an upgrade
# bounced off it, waited fifteen seconds for a sink and fell back. Every user
# who ran the old version had it; nobody was ever told to clear it by hand.
#
# Safe here: unmasking starts nothing, and furios-audio-apply.service runs
# Before=pulseaudio.service pipewire.service at the next login and writes the
# stored profile's masks under $HOME before anything starts.
sudo /usr/local/bin/audioctl migrate

systemctl --user daemon-reload
systemctl --user enable furios-audio-apply.service furios-audio-verify.service

echo "== plugin, monitor and configuration into the system"
./install-hal.sh

echo
echo "Done. Active profile unchanged: $(cat /var/lib/furios-audio/profile)"
