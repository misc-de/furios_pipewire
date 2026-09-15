#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Experiment: turn on MediaTek's handsfree echo suppression (DMNR) for calls.
#
# Background
# ----------
# On this device the vendor's tuning file says:
#
#   MTK_DUAL_MIC_SUPPORT         yes   two microphones are present
#   MTK_HANDSFREE_DMNR_SUPPORT   yes   the chip can do handsfree DMNR
#   MTK_INCALL_HANDSFREE_DMNR    no    during a call it is TURNED OFF
#   MTK_VOIP_HANDSFREE_DMNR      no
#   MTK_VOIP_NORMAL_DMNR         no
#
# DMNR is MediaTek's dual-microphone method against ambient noise and echo.
# With it off, the far end hears itself - especially on speakerphone.
#
# Why a bind mount
# ----------------
# /android/vendor is mounted read-only and protected by dm-verity. Tampering
# with it can, in the worst case, leave the device unbootable. A bind mount
# instead lays a modified copy over the file - the partition stays untouched,
# and a reboot clears everything away.
#
# It only takes effect once the HAL re-reads the file, i.e. on the next start
# of the audio stack (which this script takes care of).
#
# Remembering it across a reboot
# ------------------------------
# A bind mount is gone after a reboot by construction, so "set on" writes a
# marker and the boot unit lays the file over again. Two things about where
# things live follow from that, and neither is arbitrary:
#
#   - the marker is /etc/..., root-owned. It decides what the system mounts at
#     boot, so it must not be writable by the account whose audio it is.
#   - the copy is rebuilt from the vendor original on every apply, into /run,
#     which is tmpfs and root-owned. It used to sit in /var/lib/furios-audio,
#     which this user can write: mounting that at boot would have let anything
#     running as this user put its own file over a vendor one, automatically,
#     without ever asking for a password. Rebuilding costs a sed and closes it.
set -e

ORIG=/android/vendor/etc/audio_param/AudioParamOptions.xml
RUNDIR=${DMNR_RUNDIR:-/run/furios-audio-dmnr}
COPY="$RUNDIR/AudioParamOptions.dmnr.xml"
MARKER=${DMNR_MARKER:-/etc/furios-audio-dmnr.persistent}

# As root an override that moves where this reads or writes would be a way of
# mounting anything over a vendor file at boot. The tests need them and
# therefore run unprivileged.
if [ "$(id -u)" -eq 0 ]; then
    for _v in DMNR_RUNDIR DMNR_MARKER DMNR_ORIG; do
        if [ -n "$(eval echo "\${$_v:-}")" ]; then
            echo "refusing to honour $_v as root" >&2
            exit 3
        fi
    done
fi

show() {
    # First line deliberately machine-readable - the switcher app reads it.
    if grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        printf 'state=on\n'
    else
        printf 'state=off\n'
    fi
    # Second machine-readable line, same reason as the first: the app shows
    # whether this survives a reboot, and guessing from state= would be wrong.
    if [ -e "$MARKER" ]; then
        printf 'persistent=yes\n'
    else
        printf 'persistent=no\n'
    fi
    printf 'file:  %s\n' "$ORIG"
    if mountpoint -q "$ORIG" 2>/dev/null || grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        printf 'state: modified copy is laid over it\n'
    else
        printf 'state: vendor original\n'
    fi
    printf 'current values:\n'
    grep -oE '<Param name="(MTK_INCALL_HANDSFREE_DMNR|MTK_VOIP_HANDSFREE_DMNR|MTK_VOIP_NORMAL_DMNR|MTK_HANDSFREE_DMNR_SUPPORT|MTK_DUAL_MIC_SUPPORT)" value="[^"]*"' "$ORIG" \
        | sed 's/<Param name="/  /; s/" value="/ = /; s/"$//'
}

# The copy is built here and nowhere else, always from the vendor original,
# into a root-owned directory on tmpfs. Never from a file left lying around:
# what gets laid over a vendor file has to be something this script made.
baue_kopie() {
    [ -r "$ORIG" ] || { echo "tuning file not readable - wrong device?" >&2; return 1; }
    sudo mkdir -p "$RUNDIR"
    sudo chmod 0755 "$RUNDIR"
    sed -e 's/<Param name="MTK_INCALL_HANDSFREE_DMNR" value="no"/<Param name="MTK_INCALL_HANDSFREE_DMNR" value="yes"/' \
        -e 's/<Param name="MTK_VOIP_HANDSFREE_DMNR" value="no"/<Param name="MTK_VOIP_HANDSFREE_DMNR" value="yes"/' \
        -e 's/<Param name="MTK_VOIP_NORMAL_DMNR" value="no"/<Param name="MTK_VOIP_NORMAL_DMNR" value="yes"/' \
        "$ORIG" | sudo tee "$COPY" >/dev/null
    if sudo cmp -s "$ORIG" "$COPY"; then
        echo "nothing to change - the switches are not set to 'no'." >&2
        sudo rm -f "$COPY"; return 1
    fi
}

einschalten() {
    grep -q " $ORIG " /proc/mounts 2>/dev/null && return 0   # already over it
    baue_kopie || return 1
    sudo mount --bind "$COPY" "$ORIG"
}

ausschalten() {
    if grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        sudo umount "$ORIG"
        return 0
    fi
    return 1
}

case "${1:-status}" in
status) show ;;

on)
    einschalten || exit 1
    echo "Modified copy mounted. Restarting the audio stack so the HAL reads it:"
    audioctl restart >/dev/null 2>&1 || true
    echo
    show
    echo
    echo "Now place a call and ask the far end whether the echo is gone."
    echo "Back: $0 off   (or simply reboot)"
    ;;

set)
    # Same words as audioctl and gpsctl: "on"/"off" is now, "set" is now and
    # after the next reboot as well.
    case "${2:-}" in
    on)
        einschalten || exit 1
        printf 'on\n' | sudo tee "$MARKER" >/dev/null
        sudo chmod 0644 "$MARKER"
        audioctl restart >/dev/null 2>&1 || true
        echo "On, and remembered - the boot unit lays it over again."
        ;;
    off)
        ausschalten || true
        sudo rm -f "$MARKER"
        audioctl restart >/dev/null 2>&1 || true
        echo "Off, and remembered - nothing is laid over at boot."
        ;;
    *) echo "usage: $0 set on|off" >&2; exit 1 ;;
    esac
    show
    ;;

boot)
    # What the boot unit runs. Quiet and never fatal: echo suppression that
    # cannot be put in place is not a reason to hold up the boot, and this
    # runs before anybody could be in a call anyway.
    [ -e "$MARKER" ] || exit 0
    einschalten || exit 0
    ;;

off)
    if ausschalten; then
        echo "Original restored."
    else
        echo "Nothing was laid over it."
    fi
    # The marker is left alone on purpose: "off" is for now, "set off" is for
    # good. Say so, rather than let the next boot look like it undid this.
    [ -e "$MARKER" ] && echo "Still remembered - it comes back at the next boot ($0 set off)."
    audioctl restart >/dev/null 2>&1 || true
    show
    ;;

*) echo "usage: $0 [status|on|off|set on|set off|boot]" >&2; exit 1 ;;
esac
