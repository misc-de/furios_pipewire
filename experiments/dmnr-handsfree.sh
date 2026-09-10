#!/bin/bash
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
set -e

ORIG=/android/vendor/etc/audio_param/AudioParamOptions.xml
COPY=/var/lib/furios-audio/AudioParamOptions.dmnr.xml

show() {
    # First line deliberately machine-readable - the switcher app reads it.
    if grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        printf 'state=on\n'
    else
        printf 'state=off\n'
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

case "${1:-status}" in
status) show ;;

on)
    [ -r "$ORIG" ] || { echo "tuning file not readable - wrong device?" >&2; exit 1; }
    mkdir -p "$(dirname "$COPY")"
    sed -e 's/<Param name="MTK_INCALL_HANDSFREE_DMNR" value="no"/<Param name="MTK_INCALL_HANDSFREE_DMNR" value="yes"/' \
        -e 's/<Param name="MTK_VOIP_HANDSFREE_DMNR" value="no"/<Param name="MTK_VOIP_HANDSFREE_DMNR" value="yes"/' \
        -e 's/<Param name="MTK_VOIP_NORMAL_DMNR" value="no"/<Param name="MTK_VOIP_NORMAL_DMNR" value="yes"/' \
        "$ORIG" > "$COPY"
    if cmp -s "$ORIG" "$COPY"; then
        echo "nothing to change - the switches are not set to 'no'." >&2
        rm -f "$COPY"; exit 1
    fi
    sudo mount --bind "$COPY" "$ORIG"
    echo "Modified copy mounted. Restarting the audio stack so the HAL reads it:"
    audioctl restart >/dev/null 2>&1 || true
    echo
    show
    echo
    echo "Now place a call and ask the far end whether the echo is gone."
    echo "Back: $0 off   (or simply reboot)"
    ;;

off)
    if grep -q " $ORIG " /proc/mounts 2>/dev/null; then
        sudo umount "$ORIG"
        echo "Original restored."
    else
        echo "Nothing was laid over it."
    fi
    audioctl restart >/dev/null 2>&1 || true
    show
    ;;

*) echo "usage: $0 [status|on|off]" >&2; exit 1 ;;
esac
