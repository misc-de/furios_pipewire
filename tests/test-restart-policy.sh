#!/bin/bash
# SPDX-FileCopyrightText: Copyright (c) 2026 misc-de
# SPDX-License-Identifier: MIT
#
# A service that restarts must never be able to loop at full speed for ever.
#
# The trap is that doing nothing looks like doing something. systemd's default
# is five starts in ten seconds, so a unit with RestartSec=5 fits two of them
# into the window and a unit with RestartSec=10 exactly one - the limit is
# there, it is simply unreachable, and a permanently broken service restarts
# every few seconds for the uptime of the phone. That is battery, and it is
# invisible: nothing fails, nothing is logged beyond the restarts themselves.
#
# So every restarting unit has to say which of two things it wants:
#
#   a reachable limit   StartLimitBurst * RestartSec < StartLimitIntervalSec
#                       for services where giving up is safe.
#
#   no limit + backoff  StartLimitIntervalSec=0 together with RestartSteps and
#                       RestartMaxDelaySec, for the supervisors - where giving
#                       up is the expensive failure, because whatever they
#                       watch then goes unwatched until the next boot. The
#                       backoff is what keeps that from costing anything: the
#                       delay stretches until a permanent fault settles at one
#                       start every few minutes.
#
# Two more things systemd swallows quietly, both checked here: StartLimit* put
# in [Service] is parsed and dropped (it belongs in [Unit]), and
# RestartMaxDelaySec without RestartSteps is ignored with a warning nobody
# reads.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
. "$HERE/lib.sh"

UNITS="$ROOT"

value() { sed -n "s/^$2=//p" "$1" | tail -1; }

found=0
for u in "$UNITS"/*.service; do
    [ -f "$u" ] || continue
    name=$(basename "$u")

    # Nothing to check where nothing restarts.
    grep -qE '^Restart=(always|on-failure|on-abnormal)' "$u" || continue
    found=$((found + 1))

    # [Unit] only, so a key sitting in [Service] reads as absent - which is
    # exactly what systemd makes of it.
    unit_part=$(sed -n '/^\[Unit\]/,/^\[Service\]/p' "$u")
    window=$(printf '%s\n' "$unit_part" | sed -n 's/^StartLimitIntervalSec=//p' | tail -1)
    burst=$(printf '%s\n' "$unit_part" | sed -n 's/^StartLimitBurst=//p' | tail -1)

    delay=$(value "$u" RestartSec)
    steps=$(value "$u" RestartSteps)
    maxdelay=$(value "$u" RestartMaxDelaySec)

    check "$name: StartLimit* is in [Unit], not in [Service]" "0" \
        "$(if grep -q '^StartLimit' "$u" && [ -z "$window$burst" ]; then echo 1; else echo 0; fi)"

    check "$name: RestartSec is set" "yes" \
        "$([ -n "$delay" ] && echo yes || echo no)"

    if [ "$window" = "0" ]; then
        check "$name: no limit, backoff instead (RestartSteps)" "yes" \
            "$([ -n "$steps" ] && echo yes || echo no)"
        check "$name: and a ceiling for it (RestartMaxDelaySec)" "yes" \
            "$([ -n "$maxdelay" ] && echo yes || echo no)"
    else
        check "$name: a restart policy is set at all" "yes" \
            "$([ -n "$window" ] && [ -n "$burst" ] && echo yes || echo no)"
        if [ -n "$window" ] && [ -n "$burst" ] && [ -n "$delay" ]; then
            check "$name: the limit is reachable ($burst x ${delay}s < ${window}s)" "yes" \
                "$([ $((burst * delay)) -lt "$window" ] && echo yes || echo no)"
        fi
    fi

    # systemd: "Service has RestartMaxDelaySec= but no RestartSteps= setting.
    # Ignoring." - silently, in the journal, once.
    check "$name: RestartMaxDelaySec not without RestartSteps" "0" \
        "$(if [ -n "$maxdelay" ] && [ -z "$steps" ]; then echo 1; else echo 0; fi)"
done

check "units were checked at all" "yes" \
    "$([ "$found" -gt 0 ] && echo yes || echo no)"

summary
