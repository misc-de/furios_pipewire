# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
# A test harness small enough to read in one sitting.
#
# No framework on purpose: this has to run on the phone itself, where every
# extra dependency is one more thing that can be missing at the moment you
# need the tests most.

TESTS_RUN=0
TESTS_FAILED=0

ok()   { printf '  \033[32mok\033[0m   %s\n' "$1"; }
fail() { printf '  \033[31mFAIL\033[0m %s\n' "$1"; [ -n "${2:-}" ] && printf '       %s\n' "$2"; }

check() {
    # check <description> <expected> <actual>
    TESTS_RUN=$((TESTS_RUN + 1))
    if [ "$2" = "$3" ]; then
        ok "$1"
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        fail "$1" "expected [$2], got [$3]"
    fi
}

check_status() {
    # check_status <description> <expected exit code> <command...>
    local desc=$1 want=$2; shift 2
    TESTS_RUN=$((TESTS_RUN + 1))
    "$@" >/dev/null 2>&1
    local got=$?
    if [ "$got" = "$want" ]; then
        ok "$desc"
    else
        TESTS_FAILED=$((TESTS_FAILED + 1))
        fail "$desc" "expected exit $want, got $got"
    fi
}

summary() {
    printf '\n  %d checks, %d failed\n' "$TESTS_RUN" "$TESTS_FAILED"
    [ "$TESTS_FAILED" -eq 0 ]
}

# Put a stub on PATH that prints whatever the test wants it to print.
#
# The point is to pin down what audioctl does with the *output* of pactl and
# systemctl, without a sound server anywhere near it.
make_stub() {
    # make_stub <name> <exit code> <stdout...>
    #
    # No output means no output - not an empty line. The difference matters:
    # "pactl list sinks short" with nothing to list prints zero bytes, and a
    # stub that prints a blank line instead would let an empty-line-tolerant
    # check pass a test it should fail.
    local name=$1 code=$2; shift 2
    {
        printf '#!/bin/sh\n'
        if [ -n "$*" ]; then
            printf "cat <<'OUT'\n%s\nOUT\n" "$*"
        fi
        printf 'exit %s\n' "$code"
    } > "$STUBDIR/$name"
    chmod +x "$STUBDIR/$name"
}
