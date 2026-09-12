#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Installs the switching infrastructure. Changes nothing about the running
# audio stack - after installation the 'standard' profile is still active.
set -e
cd "$(dirname "$0")"
sudo mkdir -p /usr/local/share/furios-audio /usr/local/bin
sudo install -m755 audioctl                  /usr/local/bin/audioctl
sudo install -m644 tunnel.conf               /usr/local/share/furios-audio/tunnel.conf
sudo install -m644 furios-pw-tunnel.service  /etc/systemd/user/furios-pw-tunnel.service
sudo install -m644 furios-audio-apply.service /etc/systemd/user/furios-audio-apply.service
sudo install -m644 furios-audio-verify.service /etc/systemd/user/furios-audio-verify.service
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
systemctl --user daemon-reload
systemctl --user enable furios-audio-apply.service furios-audio-verify.service
echo
echo "Done. Active profile unchanged: $(cat /var/lib/furios-audio/profile)"
echo "Check with:  audioctl status"
