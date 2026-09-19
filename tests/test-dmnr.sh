#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
#
# What can be decided about the echo suppression without root and without the
# vendor's tuning file.
#
# What is NOT here, and cannot be: whether the far end stops hearing itself.
# That needs a call and somebody on the other side. What is here is everything
# around it - that the setting can be remembered, that the boot path keeps
# quiet when it was not, and that the thing laid over a vendor file is always
# one this script just built.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
. "$HERE/lib.sh"

TOOL=$ROOT/experiments/dmnr-handsfree.sh
UNIT=$ROOT/systemd/furios-audio-dmnr.service
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

# Stand-ins for the vendor's tuning files, with the switches in the state the
# device ships them in - and two of them, because that is the whole point: the
# parser reads a base file and a vendor extension, and a copy laid over the
# base alone leaves the extension saying "no".
mkdir -p "$TMP/param"
cat > "$TMP/param/AudioParamOptions.xml" <<'XML'
<Params>
<Param name="MTK_DUAL_MIC_SUPPORT" value="yes"/>
<Param name="MTK_HANDSFREE_DMNR_SUPPORT" value="yes"/>
<Param name="MTK_INCALL_HANDSFREE_DMNR" value="no"/>
<Param name="MTK_VOIP_HANDSFREE_DMNR" value="no"/>
<Param name="MTK_VOIP_NORMAL_DMNR" value="no"/>
</Params>
XML
cat > "$TMP/param/AudioParamOptions_vext.xml" <<'XML'
<Params>
<Param name="MTK_DUAL_MIC_SUPPORT" value="yes"/>
<Param name="MTK_HANDSFREE_DMNR_SUPPORT" value="yes"/>
<Param name="MTK_INCALL_HANDSFREE_DMNR" value="no"/>
<Param name="MTK_VOIP_HANDSFREE_DMNR" value="no"/>
<Param name="MTK_VOIP_NORMAL_DMNR" value="no"/>
<Param name="MTK_INCALL_NORMAL_DMNR" value=""/>
</Params>
XML
sed "s|^PARAMDIR=.*|PARAMDIR=$TMP/param|" "$TOOL" > "$TMP/dmnr.sh"

lauf() { DMNR_RUNDIR="$TMP/run" DMNR_MARKER="$TMP/marker" bash "$TMP/dmnr.sh" "$@" 2>&1; }

# The app reads these two lines and shows four states from them. Both have to
# be there, and on their own line, or it shows the wrong one.
check "status reports whether it is on" "1" "$(lauf status | grep -c '^state=')"
check "and whether it is remembered" "1" "$(lauf status | grep -c '^persistent=')"
check "nothing remembered to begin with" "persistent=no" "$(lauf status | sed -n 2p)"

# The boot unit runs this on every boot, on every device. Without a marker it
# has to be a no-op that succeeds - a failure here would show up as a failed
# unit on a phone where nobody ever asked for echo suppression.
lauf boot >/dev/null 2>&1
check "boot without a marker does nothing, successfully" "0" "$?"
check "and lays nothing over the files" "no" \
    "$(ls "$TMP/run" 2>/dev/null | grep -q . && echo yes || echo no)"

# The regression this file exists for: the first version of the tool changed
# AudioParamOptions.xml and nothing else, the HAL went on reading "no" out of
# AudioParamOptions_vext.xml, and the switch in the app did nothing that could
# be heard. Both files have to be seen, and a half state has to say off.
check "every options file the parser reads is seen" "2" \
    "$(lauf status | grep -c '^file:')"
check "the vendor extension is one of them" "1" \
    "$(lauf status | grep -c 'AudioParamOptions_vext.xml')"
check "with nothing laid over, that is off" "state=off" "$(lauf status | sed -n 1p)"
check "the switch for a call held to the ear is set too" "yes" \
    "$(grep -q 'MTK_INCALL_NORMAL_DMNR' "$TOOL" && echo yes || echo no)"

touch "$TMP/marker"
check "a marker is seen" "persistent=yes" "$(lauf status | sed -n 2p)"

# "off" is for now and "set off" is for good. If "off" dropped the marker, the
# two would be the same command and the distinction the app relies on would
# not exist.
check "'off' leaves the marker alone" "yes" \
    "$([ -e "$TMP/marker" ] && echo yes || echo no)"
check "and says the reboot will bring it back" "1" \
    "$(lauf off | grep -c 'comes back at the next boot')"

check "an unknown word is refused" "1" \
    "$(lauf quatsch >/dev/null 2>&1; echo $?)"
check "and 'set' without on/off too" "1" \
    "$(lauf set >/dev/null 2>&1; echo $?)"

# The copy is rebuilt from the vendor original every time, into a root-owned
# directory on tmpfs. It used to be written into /var/lib/furios-audio, which
# this user can write - and with a boot unit mounting it, anything running as
# this user could have put its own file over a vendor one at the next boot,
# without a password ever being asked for.
check "the copy is built in /run, not somewhere this user owns" "yes" \
    "$(grep -q 'RUNDIR=${DMNR_RUNDIR:-/run/' "$TOOL" && echo yes || echo no)"
check "and always rebuilt from the vendor file, never reused" "yes" \
    "$(grep -q 'baue_kopie()' "$TOOL" && grep -q 'einschalten()' "$TOOL" && echo yes || echo no)"
check "the marker lives where this user cannot write it" "yes" \
    "$(grep -q 'MARKER=${DMNR_MARKER:-/etc/' "$TOOL" && echo yes || echo no)"
check "as root the overrides are refused" "yes" \
    "$(grep -q 'refusing to honour' "$TOOL" && echo yes || echo no)"

# The unit is what makes "remembered" true, so it has to exist, be valid, and
# be ordered before anything reads the file it lays over.
check "there is a boot unit" "yes" "$([ -f "$UNIT" ] && echo yes || echo no)"
check "it runs before the session starts the audio stack" "1" \
    "$(grep -c '^Before=graphical.target' "$UNIT")"
check "it does nothing on a device without the vendor file" "1" \
    "$(grep -c '^ConditionPathExists=' "$UNIT")"

# The condition and the mount, in that order - and the order is the whole
# point. A ConditionPathExists is checked when the unit is about to start, so
# without this the unit was skipped at every boot: the path it asks about is
# inside the vendor image, which android-mount.service was still mounting.
# Measured 2026-09-19: unmet at 12:52:20, mounted at 12:52:56, the setting
# gone while status still said persistent=yes.
check "it waits for the vendor image before asking whether the file is there" "1" \
    "$(grep -c '^After=android-mount.service' "$UNIT")"
# ... and the condition is about a path under that very mount, which is what
# ties the two lines together.
check "and the path it asks about is under that mount" "yes" \
    "$(grep '^ConditionPathExists=' "$UNIT" | grep -q '=/android/' && echo yes || echo no)"
if command -v systemd-analyze >/dev/null 2>&1; then
    check "and systemd accepts every key in it" "" \
        "$(systemd-analyze verify "$UNIT" 2>&1 | grep -iE 'unknown key|unknown lvalue' | head -1)"
fi

# Installed and removed as a pair. A marker left behind by an uninstall would
# mount a file at boot that nothing on the system knows about any more.
check "install-hal.sh installs the unit" "yes" \
    "$(grep -q 'furios-audio-dmnr.service' "$ROOT/install-hal.sh" && echo yes || echo no)"
check "the package ships it too" "yes" \
    "$(grep -q 'furios-audio-dmnr.service' "$ROOT/packaging/build-deb.sh" && echo yes || echo no)"
check "uninstall.sh removes the unit" "yes" \
    "$(grep -q 'furios-audio-dmnr.service' "$ROOT/uninstall.sh" && echo yes || echo no)"
check "and the marker with it" "yes" \
    "$(grep -q 'furios-audio-dmnr.persistent' "$ROOT/uninstall.sh" && echo yes || echo no)"

summary
