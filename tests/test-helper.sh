# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# The part that runs as root.
#
# This is the only code in the project that gets root, so what matters is not
# that it works but what it refuses. It takes no paths from its caller at all
# and knows five operations; every test here is about the edges of that.
set -u
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

HELPER="$ROOT/tools/furios-audio-helper"
export FURIOS_AUDIO_HELPER_ROOT="$STUBDIR"
ETCU="$STUBDIR/etc/systemd/user"
WPDIR="$STUBDIR/usr/share/wireplumber/wireplumber.conf.d"
HALDIR="$STUBDIR/usr/share/furios-audio"
mkdir -p "$ETCU" "$WPDIR" "$HALDIR"

# Run it through bash so AUDIOCTL_TRACE applies; the shebang says sh, and here
# that is dash, which cannot report line numbers.
helper() { bash ${AUDIOCTL_TRACE:+-x} "$HELPER" "$@"; }

printf 'the privileged helper\n'

printf '\nwhat it refuses\n'
check_status "a unit that is not ours is not masked" 1 helper mask sshd.service
check_status "and not unmasked either" 1 helper unmask sshd.service
check_status "an operation it does not know" 1 helper nonsense
check_status "mask without a unit" 1 helper mask
check_status "unmask without a unit" 1 helper unmask
check_status "wpconf without on or off" 1 helper wpconf
check_status "wpconf with something else" 1 helper wpconf sideways
check_status "nothing at all" 1 helper
check "a refused unit is not created on the way out" "no" \
    "$([ -e "$ETCU/sshd.service" ] && echo yes || echo no)"

printf '\nmasking, which is a link to /dev/null\n'
check_status "our own units are masked" 0 helper mask pulseaudio.service pulseaudio.socket
check "the link is there" "/dev/null" "$(readlink "$ETCU/pulseaudio.service")"
check "for both of them" "/dev/null" "$(readlink "$ETCU/pulseaudio.socket")"
check_status "masking again is not an error" 0 helper mask pulseaudio.service
check_status "and they can be unmasked" 0 helper unmask pulseaudio.service pulseaudio.socket
check "the link is gone" "no" \
    "$([ -e "$ETCU/pulseaudio.service" ] && echo yes || echo no)"
check_status "unmasking something that is not masked is not an error" 0 \
    helper unmask pulseaudio.service

# A real file with that name is somebody else's, not a mask of ours.
printf '\na file that only looks like a mask\n'
echo "not ours" > "$ETCU/wireplumber.service"
check_status "it is left alone" 0 helper unmask wireplumber.service
check "and still there" "not ours" "$(cat "$ETCU/wireplumber.service")"
ln -sf /etc/passwd "$ETCU/wireplumber.service"
check_status "a link somewhere else is left alone too" 0 helper unmask wireplumber.service
check "and still points where it did" "/etc/passwd" \
    "$(readlink "$ETCU/wireplumber.service")"
rm -f "$ETCU/wireplumber.service"

printf '\nthe systemd drop-in\n'
check_status "without a configuration there is nothing to point at" 1 helper dropin-write
echo "# hal conf" > "$HALDIR/pipewire-hal.conf"
check_status "with one it is written" 0 helper dropin-write
DROPIN="$ETCU/pipewire.service.d/50-furios-audio.conf"
check "it starts pipewire with that configuration" "yes" \
    "$(grep -q "$HALDIR/pipewire-hal.conf" "$DROPIN" && echo yes || echo no)"
check "and clears the shipped ExecStart first, or systemd would run both" "yes" \
    "$(grep -qx 'ExecStart=' "$DROPIN" && echo yes || echo no)"
check_status "and it can be removed" 0 helper dropin-remove
check "then it is gone" "no" "$([ -e "$DROPIN" ] && echo yes || echo no)"
check_status "removing it twice is not an error" 0 helper dropin-remove

printf '\nthe WirePlumber monitor, moved out of the way and back\n'
echo "monitor" > "$WPDIR/50-droid.conf"
check_status "it can be switched off" 0 helper wpconf off
check "which moves the file aside" "yes" \
    "$([ -e "$WPDIR/50-droid.conf.off" ] && echo yes || echo no)"
check "and leaves nothing behind" "no" \
    "$([ -e "$WPDIR/50-droid.conf" ] && echo yes || echo no)"
check_status "switching it off again is not an error" 0 helper wpconf off
check_status "and it comes back" 0 helper wpconf on
check "with its contents" "monitor" "$(cat "$WPDIR/50-droid.conf")"
check_status "switching it on again is not an error" 0 helper wpconf on

summary
