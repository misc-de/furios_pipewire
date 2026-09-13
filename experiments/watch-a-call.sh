#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Watch one whole call from the outside, so that nothing has to be typed
# during it.
#
# Why an instrument instead of a few commands
# -------------------------------------------
# Typing during a call is how this project has broken calls before: callaudiod
# switches cards at answer and at hang-up, and anything that touches a card
# while it is doing that can take the audio out for the rest of the call. So
# this is started BEFORE the phone rings, left alone, and read afterwards.
# Everything it asks is read-only.
#
# What it is for
# --------------
# The question it answers is whether the SCO hold happens by itself, in
# ordinary use, with nothing held open by hand:
#
#   1. does furios-audio-sco-hold say "holding the link" on its own,
#   2. does an SCO link actually appear in "hcitool con" - the profile being
#      right proves nothing, that was the whole lesson of 2026-09-12,
#   3. is it let go again at hang-up, and
#   4. is nothing still held a quarter of a minute later.
#
# The one thing it cannot record is whether the call could be heard. That part
# is the person holding the phone.
set -u

SECONDS_TO_WATCH=${1:-180}
LOG=${2:-${XDG_RUNTIME_DIR:-/tmp}/watch-a-call-$(date +%Y%m%d-%H%M%S).log}

say() { printf '%s %s\n' "$(date +%H:%M:%S.%2N)" "$*" >>"$LOG"; }

# The daemon's own account of what it decided, interleaved with the
# measurements by timestamp.
journalctl --user -fu furios-audio-sco-hold.service -o short-iso --since=now \
    >>"$LOG" 2>&1 &
FOLLOWER=$!

cleanup() {
    kill "$FOLLOWER" 2>/dev/null
    say "== stopped watching"
    printf '\nwritten to %s\n' "$LOG"
}
trap cleanup EXIT INT TERM

say "== watching for ${SECONDS_TO_WATCH}s"
printf 'watching a call for %ss - start it now, and do not type anything.\n' \
    "$SECONDS_TO_WATCH"
printf 'log: %s\n' "$LOG"

last=""
for _ in $(seq "$SECONDS_TO_WATCH"); do
    # The Bluetooth card's profile, if there is a Bluetooth card at all.
    profile=$(pactl list cards 2>/dev/null | awk '
        /^\tName: bluez_card\./ { in_bluez = 1; next }
        /^\tName: / { in_bluez = 0 }
        in_bluez && /^\tActive Profile: / { sub(/^\tActive Profile: /, ""); print; exit }')

    # The link itself. A profile without one of these is exactly the silent
    # call this whole thing exists to prevent.
    sco=$(hcitool con 2>/dev/null | grep -c SCO)

    # Who is holding it, if anyone. By process NAME, never by command line:
    # "pgrep -f paplay.*bluez_output" also matches the shell that is asking,
    # because the pattern is in its own command line, and then this reports a
    # hold that does not exist.
    hold=none
    for pid in $(pgrep -x paplay 2>/dev/null); do
        device=$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null |
            sed -n 's/.*--device=\([^ ]*\).*/\1/p')
        case $device in bluez_output.*) hold=$device ;; esac
    done

    # The codec actually negotiated. A hold on a link that is encoding CVSD
    # against an mSBC stream measures perfectly and is still silent.
    codec=$(pactl list cards 2>/dev/null | sed -n 's/^\t\tbluetooth.codec = "\(.*\)"/\1/p' | head -1)

    calls=$(dbus-send --system --print-reply --dest=org.ofono /ril_0 \
        org.ofono.VoiceCallManager.GetCalls 2>/dev/null | grep -c 'object path')

    sinks=$(pactl list short sinks 2>/dev/null |
        awk '{printf "%s=%s ", $2, $NF}')

    now="calls=$calls sco=$sco profile=${profile:-none} codec=${codec:-none} hold=$hold $sinks"
    # Only changes, so that a minute of a steady call is one line and the
    # moments that matter are not buried in repetition.
    if [ "$now" != "$last" ]; then
        say "$now"
        last=$now
    fi
    sleep 1
done
