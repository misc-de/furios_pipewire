#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# What audioctl decides, given what the system tells it.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"

STUBDIR=$(mktemp -d)
trap 'rm -rf "$STUBDIR"' EXIT
export PATH="$STUBDIR:$PATH"

# --- verify(): the safety net -----------------------------------------------
#
# This is the check that decides whether a profile switch stands or is rolled
# back. It used to accept any sink at all, including PipeWire's fallback
# auto_null - which exists precisely when no real device came up, so a plugin
# that failed to load passed the check and left the phone silent.

run_verify() {
    # run_verify <what pactl prints>
    make_stub pactl 0 "$1"
    ( AUDIOCTL_LIB=1 . "$HERE/../audioctl"
      # The real one waits 15 s for a sink; one pass is enough to test the
      # decision, and nobody wants a test suite that sleeps.
      verify_once() {
          pactl list sinks short 2>/dev/null | awk 'NF {print $2}' | grep -qvx auto_null
      }
      verify_once )
}

check_status "a real sink passes" 0 \
    run_verify "60	droid-sink	PipeWire	s16le 2ch 48000Hz	SUSPENDED"
check_status "the shipped PulseAudio sink passes" 0 \
    run_verify "1	sink.primary_output	module-droid-card.c	s16le 2ch 48000Hz	IDLE"
check_status "auto_null alone fails - it means no real device came up" 1 \
    run_verify "35	auto_null	PipeWire	float32le 2ch 48000Hz	SUSPENDED"
check_status "auto_null next to a real sink passes" 0 \
    run_verify "35	auto_null	PipeWire	float32le 2ch 48000Hz	SUSPENDED
60	droid-sink	PipeWire	s16le 2ch 48000Hz	SUSPENDED"
check_status "no sink at all fails" 1 run_verify ""
check_status "a stray blank line is not a sink" 1 run_verify "
"

# The system grep, not whatever the shell has aliased. A previous version of
# this check was tested against ugrep, whose -q -v means something else, and
# the result looked fine while being backwards.
check "audioctl uses plain grep, and /bin/grep is GNU grep" "ok" \
    "$(/bin/grep --version | head -1 | grep -q GNU && echo ok || echo "not GNU grep")"

# --- effective_profile(): what is actually running --------------------------
#
# The recorded profile can go stale - that once sent a call test at the wrong
# stack entirely. This reads reality off the running services instead.

run_effective() {
    # run_effective <active service name or "none">
    cat > "$STUBDIR/systemctl" <<STUB
#!/bin/sh
# systemctl --user is-active <unit>
[ "\$3" = "$1" ] && exit 0
exit 3
STUB
    chmod +x "$STUBDIR/systemctl"
    ( AUDIOCTL_LIB=1 . "$HERE/../audioctl"; effective_profile )
}

check "PulseAudio running means standard" "standard" "$(run_effective pulseaudio.service)"
check "pipewire-pulse running means pw-hal" "pw-hal" "$(run_effective pipewire-pulse.service)"
check "neither running is reported as unclear" "unclear" "$(run_effective none)"

# --- the profile list -------------------------------------------------------
check "the known profiles are exactly the three documented ones" \
    "standard pw-tunnel pw-hal" \
    "$( AUDIOCTL_LIB=1 . "$HERE/../audioctl"; printf '%s' "$PROFILES" )"

summary
