# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Do the names in our WirePlumber configuration exist?
#
# A setting or a property that nobody knows is not an error anywhere:
# WirePlumber reads the file, finds a key it has no schema for, and carries on
# with the default. The sound then behaves as if the file were not there, and
# nothing says why. That is not hypothetical - "bluez5.autoswitch-profile"
# looked exactly right and does not exist in this build; the real name lives in
# WirePlumber's settings schema, not in the bluez5 plugin.
#
# So this checks our two configuration files against what is actually
# installed: settings against WirePlumber's own schema, monitor properties
# against the strings in libspa-bluez5. Neither installed means the checks are
# skipped rather than guessed at.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
. "$HERE/lib.sh"

SCHEMA=/usr/share/wireplumber/wireplumber.conf
BLUEZ=$(ls /usr/lib/*/spa-0.2/bluez5/libspa-bluez5.so 2>/dev/null | head -1)

# Keys from a named block of an SPA-JSON file, without a JSON parser: the
# blocks here are one level deep and one key per line, which is what the files
# actually look like. Comments and the closing brace are dropped.
keys_in_block() {
    # keys_in_block <file> <block name>
    awk -v block="$2" '
        $0 ~ "^" block " *= *\\{" { inside = 1; next }
        inside && /^}/            { inside = 0 }
        inside && /^ *#/          { next }
        inside && /=/             { sub(/^ */, ""); sub(/ *=.*/, ""); print }
    ' "$1"
}

printf 'wireplumber configuration\n'

printf '\nsettings we ask for exist\n'
if [ -f "$SCHEMA" ]; then
    for f in "$ROOT"/wireplumber/*.conf; do
        for key in $(keys_in_block "$f" "wireplumber.settings"); do
            TESTS_RUN=$((TESTS_RUN + 1))
            # Either WirePlumber knows it, or we declare it ourselves in the
            # same place - a setting of our own needs a schema entry too, or
            # reading it comes back empty.
            if grep -q "^  $key = {" "$SCHEMA"; then
                ok "$(basename "$f"): $key is in WirePlumber's schema"
            elif keys_in_block "$f" "wireplumber.settings.schema" | grep -qx "$key"; then
                ok "$(basename "$f"): $key is declared in the same file"
            else
                TESTS_FAILED=$((TESTS_FAILED + 1))
                fail "$(basename "$f"): $key is known" \
                     "in neither WirePlumber's schema nor this file - it would be ignored without a word"
            fi
        done
    done
else
    printf '  \033[33mskipped\033[0m - WirePlumber not installed\n'
fi

printf '\nbluez properties we set exist\n'
if [ -n "$BLUEZ" ]; then
    for key in $(keys_in_block "$ROOT/wireplumber/51-bluez-ofono.conf" "monitor.bluez.properties"); do
        if strings "$BLUEZ" | grep -qx "$key"; then
            ok "$key is known to libspa-bluez5"
            TESTS_RUN=$((TESTS_RUN + 1))
        else
            TESTS_RUN=$((TESTS_RUN + 1))
            TESTS_FAILED=$((TESTS_FAILED + 1))
            fail "$key is known to libspa-bluez5" \
                 "no such property - it would be ignored without a word"
        fi
    done
else
    printf '  \033[33mskipped\033[0m - libspa-bluez5 not installed\n'
fi

# The one that matters most, spelled out: with this on, every recorder lists a
# Bluetooth microphone that delivers digital silence on this device.
printf '\nthe headset microphone stays out of the way\n'
val=$(keys_in_block "$ROOT/wireplumber/51-bluez-ofono.conf" "wireplumber.settings" >/dev/null
      awk -F'= *' '/bluetooth.autoswitch-to-headset-profile/ && !/^ *#/ {print $2}' \
          "$ROOT/wireplumber/51-bluez-ofono.conf")
check "the always-show-microphone setting is off" "false" "$val"

summary
