# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Giving callaudiod a fresh view of the card.
#
# The decisions are small and all of them cost a call once: do not touch it
# during one, stop it when its card is gone, and start it again with a method
# that asks nothing of it - because if SelectMode is what starts it, that
# blocks too.
set -u
# Same trace seam as the audioctl test: with AUDIOCTL_TRACE set, every line the
# script is about to run is written there, which is what coverage-shell.sh
# counts.
if [ -n "${AUDIOCTL_TRACE:-}" ]; then
    exec {trace_fd}>>"$AUDIOCTL_TRACE"
    export BASH_XTRACEFD=$trace_fd
    export PS4='+ ${BASH_SOURCE}:${LINENO}: '
fi
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
. "$HERE/lib.sh"

STUBDIR=$(mktemp -d)
trap 'rm -rf "$STUBDIR"' EXIT
PATH="$STUBDIR:$PATH"

REFRESH="$ROOT/tools/furios-audio-callaudio-refresh"

# Run it through bash so the trace above applies; the shebang says sh, and on
# this phone that is dash, which cannot report line numbers.
refresh() { bash ${AUDIOCTL_TRACE:+-x} "$REFRESH"; }

card_in() {
    # card_in <profile> - what pactl reports for the phone card
    make_stub pactl 0 "Card #59
	Name: droid
	Active Profile: $1
Card #99"
}

printf 'callaudiod refresh\n'

printf '\nduring a call it keeps its hands off\n'
card_in voicecall
make_recording_stub pkill 0 ""
make_recording_stub busctl 0 "u 0"
out=$(refresh)
check "it says so" "a call is running - leaving callaudiod alone" "$out"
check "callaudiod is not stopped" "no" \
    "$([ -e "$STUBDIR/pkill.args" ] && echo yes || echo no)"
check "and nothing is asked of it" "no" \
    "$([ -e "$STUBDIR/busctl.args" ] && echo yes || echo no)"

printf '\noutside a call it starts callaudiod again\n'
card_in default
rm -f "$STUBDIR/pkill.args" "$STUBDIR/busctl.args"
make_recording_stub pkill 0 ""
make_recording_stub busctl 0 "u 0"
out=$(refresh)
check "the old one is stopped" "yes" \
    "$(grep -q callaudiod "$STUBDIR/pkill.args" && echo yes || echo no)"
check "and a new one is started by reading its state" "yes" \
    "$(grep -q AudioMode "$STUBDIR/busctl.args" && echo yes || echo no)"
check "never by asking it to switch mode - that is what blocks" "yes" \
    "$(grep -q SelectMode "$STUBDIR/busctl.args" && echo no || echo yes)"
check "it reports what it did" "yes" \
    "$(printf '%s' "$out" | grep -q "card as it is now" && echo yes || echo no)"

printf '\nwhen there was nothing to stop\n'
card_in default
rm -f "$STUBDIR/busctl.args"
make_stub pkill 1 ""
make_recording_stub busctl 0 "u 0"
out=$(refresh)
check "it does not claim to have stopped anything" "no" \
    "$(printf '%s' "$out" | grep -q "stopped" && echo yes || echo no)"
check "but still makes sure one is running" "yes" \
    "$(grep -q AudioMode "$STUBDIR/busctl.args" && echo yes || echo no)"

printf '\nwhen the sound server is not there at all\n'
make_stub pactl 1 ""
make_stub pkill 1 ""
make_stub busctl 1 ""
check "it finishes without a fuss" "0" "$(refresh >/dev/null 2>&1; echo $?)"

summary
