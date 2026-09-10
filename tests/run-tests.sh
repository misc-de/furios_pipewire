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
run_c_test() {
    printf '\n\033[1m== spa-droid: %s\033[0m\n' "$2"
    if [ ! -d "$BUILD" ]; then
        printf '  \033[33mskipped\033[0m - no build tree, see "Building" in README.md\n'
        return
    fi
    if ninja -C "$BUILD" "$1" >/dev/null 2>&1; then
        if "$BUILD/$1"; then
            :
        else
            FAILED=$((FAILED + 1))
        fi
    else
        printf '  \033[33mskipped\033[0m - %s did not build\n' "$1"
    fi
}

run_c_test test-droid-device "card decisions"
run_c_test test-droid-pcm "the node, with the HAL replaced by a stand-in"
run_c_test test-compat "the compatibility layer"

# --- the WirePlumber scripts ------------------------------------------------
#
# They run inside a session manager, so they get one: tests/lua/ has a stub
# shallow enough to read in a sitting. Twice in one day an error in these
# scripts took WirePlumber down and every sound with it, which is the whole
# argument for testing them.
if command -v lua5.4 >/dev/null 2>&1; then
    for t in "$HERE"/lua/test-*.lua; do
        [ -e "$t" ] || continue
        name=$(basename "$t" .lua)
        printf '\n\033[1m== %s\033[0m\n' "${name#test-}"
        if TEST_ROOT="$ROOT" LUA_COVERAGE="${LUA_COVERAGE:-}" lua5.4 "$t"; then
            :
        else
            FAILED=$((FAILED + 1))
        fi
    done
else
    printf '\n\033[1m== wireplumber scripts\033[0m\n'
    printf '  \033[33mskipped\033[0m - lua5.4 not installed (apt install lua5.4)\n'
fi

# --- shell syntax, cheap and worth it ---------------------------------------
printf '\n\033[1m== shell scripts parse\033[0m\n'
for f in "$ROOT"/*.sh "$ROOT"/audioctl "$ROOT"/gui/*.sh "$ROOT"/tools/*.sh \
         "$ROOT"/experiments/*.sh "$ROOT"/packaging/*.sh "$ROOT"/tests/*.sh; do
    [ -e "$f" ] || continue
    if bash -n "$f" 2>/dev/null; then
        printf '  \033[32mok\033[0m   %s\n' "${f#$ROOT/}"
    else
        printf '  \033[31mFAIL\033[0m %s\n' "${f#$ROOT/}"
        FAILED=$((FAILED + 1))
    fi
done

printf '\n\033[1m== lua scripts parse\033[0m\n'
if command -v luac5.4 >/dev/null 2>&1; then
    for f in "$ROOT"/wireplumber/*.lua "$ROOT"/tests/lua/*.lua; do
        [ -e "$f" ] || continue
        if luac5.4 -p "$f" 2>/dev/null; then
            printf '  \033[32mok\033[0m   %s\n' "${f#$ROOT/}"
        else
            printf '  \033[31mFAIL\033[0m %s\n' "${f#$ROOT/}"
            FAILED=$((FAILED + 1))
        fi
    done
else
    printf '  \033[33mskipped\033[0m - luac5.4 not installed\n'
fi

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
