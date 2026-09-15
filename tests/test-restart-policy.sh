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

wert() { sed -n "s/^$2=//p" "$1" | tail -1; }

gefunden=0
for u in "$UNITS"/*.service; do
    [ -f "$u" ] || continue
    name=$(basename "$u")

    # Nothing to check where nothing restarts.
    grep -qE '^Restart=(always|on-failure|on-abnormal)' "$u" || continue
    gefunden=$((gefunden + 1))

    # [Unit] only, so a key sitting in [Service] reads as absent - which is
    # exactly what systemd makes of it.
    unit_teil=$(sed -n '/^\[Unit\]/,/^\[Service\]/p' "$u")
    fenster=$(printf '%s\n' "$unit_teil" | sed -n 's/^StartLimitIntervalSec=//p' | tail -1)
    burst=$(printf '%s\n' "$unit_teil" | sed -n 's/^StartLimitBurst=//p' | tail -1)

    verzug=$(wert "$u" RestartSec)
    stufen=$(wert "$u" RestartSteps)
    maxverzug=$(wert "$u" RestartMaxDelaySec)

    check "$name: StartLimit* steht in [Unit], nicht in [Service]" "0" \
        "$(if grep -q '^StartLimit' "$u" && [ -z "$fenster$burst" ]; then echo 1; else echo 0; fi)"

    check "$name: RestartSec ist gesetzt" "ja" \
        "$([ -n "$verzug" ] && echo ja || echo nein)"

    if [ "$fenster" = "0" ]; then
        check "$name: kein Limit, dafuer Backoff (RestartSteps)" "ja" \
            "$([ -n "$stufen" ] && echo ja || echo nein)"
        check "$name: und eine Obergrenze dafuer (RestartMaxDelaySec)" "ja" \
            "$([ -n "$maxverzug" ] && echo ja || echo nein)"
    else
        check "$name: eine Neustart-Politik ist ueberhaupt gesetzt" "ja" \
            "$([ -n "$fenster" ] && [ -n "$burst" ] && echo ja || echo nein)"
        if [ -n "$fenster" ] && [ -n "$burst" ] && [ -n "$verzug" ]; then
            check "$name: die Grenze ist erreichbar ($burst x ${verzug}s < ${fenster}s)" "ja" \
                "$([ $((burst * verzug)) -lt "$fenster" ] && echo ja || echo nein)"
        fi
    fi

    # systemd: "Service has RestartMaxDelaySec= but no RestartSteps= setting.
    # Ignoring." - silently, in the journal, once.
    check "$name: RestartMaxDelaySec nicht ohne RestartSteps" "0" \
        "$(if [ -n "$maxverzug" ] && [ -z "$stufen" ]; then echo 1; else echo 0; fi)"
done

check "es wurden ueberhaupt Units geprueft" "ja" \
    "$([ "$gefunden" -gt 0 ] && echo ja || echo nein)"

summary
