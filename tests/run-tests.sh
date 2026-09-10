#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# Everything that can be checked without a phone in your hand.
#
# What is here: the decisions. How a port is ranked, what the card does with a
# volume, when the safety net fires, whether the app and audioctl still speak
# the same words.
#
# What is not here, and cannot be: whether sound comes out. That needs the HAL,
# a headset, a real call - and the way to check it is to measure, not to assert
# (README, "Traps that cost time"). These tests exist so that the things which
# *can* be decided on a desk stop being rediscovered by ear.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
FAILED=0

run() {
    printf '\n\033[1m== %s\033[0m\n' "$1"
    shift
    if "$@"; then
        :
    else
        FAILED=$((FAILED + 1))
    fi
}

run "audioctl" bash "$HERE/test-audioctl.sh"
run "python: config generator, app/audioctl seam, bluetooth watcher" \
    python3 "$HERE/test-python.py"

# The C test needs a build tree. Without one, say so rather than pass quietly.
# Always rebuild it first. The test is not a default target - it is not part
# of the plugin - so a plain "ninja" leaves it untouched, and running a stale
# binary against changed sources is worse than not running it at all. That
# happened once: two new checks appeared to pass without ever being compiled.
BUILD="$ROOT/poc/spa-droid/build"
if [ -d "$BUILD" ]; then
    printf '\n\033[1m== spa-droid: card decisions\033[0m\n'
    if ninja -C "$BUILD" test-droid-device >/dev/null 2>&1; then
        if "$BUILD/test-droid-device"; then
            :
        else
            FAILED=$((FAILED + 1))
        fi
    else
        printf '  \033[33mskipped\033[0m - test-droid-device did not build\n'
    fi
else
    printf '\n\033[1m== spa-droid: card decisions\033[0m\n'
    printf '  \033[33mskipped\033[0m - no build tree, see "Building" in README.md\n'
fi

# --- shell syntax, cheap and worth it ---------------------------------------
printf '\n\033[1m== shell scripts parse\033[0m\n'
for f in "$ROOT"/*.sh "$ROOT"/audioctl "$ROOT"/gui/*.sh "$ROOT"/tools/*.sh \
         "$ROOT"/experiments/*.sh "$ROOT"/packaging/*.sh; do
    [ -e "$f" ] || continue
    if bash -n "$f" 2>/dev/null; then
        printf '  \033[32mok\033[0m   %s\n' "${f#$ROOT/}"
    else
        printf '  \033[31mFAIL\033[0m %s\n' "${f#$ROOT/}"
        FAILED=$((FAILED + 1))
    fi
done

printf '\n\033[1m== python scripts parse\033[0m\n'
for f in "$ROOT"/*.py "$ROOT"/gui/*.py "$ROOT"/tools/*.py "$ROOT"/poc/spa-droid/tools/*.py; do
    [ -e "$f" ] || continue
    if python3 -m py_compile "$f" 2>/dev/null; then
        printf '  \033[32mok\033[0m   %s\n' "${f#$ROOT/}"
    else
        printf '  \033[31mFAIL\033[0m %s\n' "${f#$ROOT/}"
        FAILED=$((FAILED + 1))
    fi
done

printf '\n'
if [ "$FAILED" -eq 0 ]; then
    printf '\033[32mall suites passed\033[0m\n'
else
    printf '\033[31m%d suite(s) failed\033[0m\n' "$FAILED"
fi
exit $([ "$FAILED" -eq 0 ] && echo 0 || echo 1)
